/*
   mkvmerge -- utility for splicing together matroska files
   from component media subtypes

   Distributed under the GPL
   see the file COPYING for details
   or visit http://www.gnu.org/copyleft/gpl.html

   $Id$

   VobSub stream reader

   Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "common.h"
#include "iso639.h"
#include "mm_io.h"
#include "output_control.h"
#include "p_vobsub.h"
#include "r_vobsub.h"
#include "subtitles.h"

using namespace std;

#define hexvalue(c) (  isdigit(c)        ? (c) - '0' \
                     : tolower(c) == 'a' ? 10        \
                     : tolower(c) == 'b' ? 11        \
                     : tolower(c) == 'c' ? 12        \
                     : tolower(c) == 'd' ? 13        \
                     : tolower(c) == 'e' ? 14        \
                     :                     15)
#define ishexdigit(s) (isdigit(s) || (strchr("abcdefABCDEF", s) != NULL))

#define PFX "vobsub_reader: "

const string vobsub_reader_c::id_string("# VobSub index file, v");

bool
vobsub_entry_c::operator < (const vobsub_entry_c &cmp) const {
  return timestamp < cmp.timestamp;
}

int
vobsub_reader_c::probe_file(mm_io_c *io,
                            int64_t size) {
  char chunk[80];

  try {
    io->setFilePointer(0, seek_beginning);
    if (io->read(chunk, 80) != 80)
      return 0;
    if (strncasecmp(chunk, id_string.c_str(), id_string.length()))
      return 0;
    io->setFilePointer(0, seek_beginning);
  } catch (...) {
    return 0;
  }
  return 1;
}

vobsub_reader_c::vobsub_reader_c(track_info_c &_ti)
  throw (error_c):
  generic_reader_c(_ti),
  delay(0) {

  try {
    idx_file = new mm_text_io_c(new mm_file_io_c(ti.fname));
  } catch (...) {
    throw error_c(PFX "Cound not open the source file.");
  }

  string sub_name = ti.fname;
  int len         = sub_name.rfind(".");
  if (0 <= len)
    sub_name.erase(len);
  sub_name += ".sub";

  try {
    sub_file = new mm_file_io_c(sub_name.c_str());
  } catch (...) {
    string emsg = PFX "Could not open the sub file '";
    emsg += sub_name;
    emsg += "'.";
    throw error_c(emsg.c_str());
  }

  idx_data = "";
  len      = id_string.length();

  string line;
  if (!idx_file->getline2(line) || !starts_with_case(line, id_string.c_str(), len) || (line.length() < (len + 1)))
    mxerror(PFX "%s: No version number found.\n", ti.fname.c_str());

  version = line[len] - '0';
  len++;
  while ((len < line.length()) && isdigit(line[len])) {
    version = version * 10 + line[len] - '0';
    len++;
  }
  if (version < 7)
    mxerror(PFX "%s: Only v7 and newer VobSub files are supported. If you have an older version then use the VSConv utility from "
            "http://sourceforge.net/projects/guliverkli/ to convert these files to v7 files.\n", ti.fname.c_str());

  parse_headers();
  if (verbose)
    mxinfo(FMT_FN "Using the VobSub subtitle reader (SUB file '%s').\n", ti.fname.c_str(), sub_name.c_str());
}

vobsub_reader_c::~vobsub_reader_c() {
  uint32_t i;

  for (i = 0; i < tracks.size(); i++) {
    mxverb(2, "r_vobsub track %u SPU size: " LLD ", overall size: " LLD ", overhead: " LLD " (%.3f%%)\n", i, tracks[i]->spu_size,
           tracks[i]->spu_size + tracks[i]->overhead, tracks[i]->overhead, (float)(100.0 * tracks[i]->overhead / (tracks[i]->overhead + tracks[i]->spu_size)));
    delete tracks[i];
  }
  delete sub_file;
  delete idx_file;
}

void
vobsub_reader_c::create_packetizer(int64_t tid) {
  if ((tracks.size() <= tid) || !demuxing_requested('s', tid) || (-1 != tracks[tid]->ptzr))
    return;

  vobsub_track_c *track = tracks[tid];
  ti.id                 = tid;
  ti.language           = tracks[tid]->language;
  track->ptzr           = add_packetizer(new vobsub_packetizer_c(this, idx_data.c_str(), idx_data.length(), ti));

  int64_t avg_duration;
  if (!track->entries.empty()) {
    avg_duration = 0;
    int k;
    for (k = 0; k < (track->entries.size() - 1); k++) {
      int64_t duration            = track->entries[k + 1].timestamp - track->entries[k].timestamp;
      track->entries[k].duration  = duration;
      avg_duration               += duration;
    }
  } else
    avg_duration = 1000000000;

  if (1 < track->entries.size())
    avg_duration /= (track->entries.size() - 1);
  track->entries[track->entries.size() - 1].duration = avg_duration;

  num_indices += track->entries.size();

  mxinfo(FMT_TID "Using the VobSub subtitle output module (language: %s).\n", ti.fname.c_str(), (int64_t)tid, track->language.c_str());
  ti.language = "";
}

void
vobsub_reader_c::create_packetizers() {
  uint32_t i;

  for (i = 0; i < tracks.size(); i++)
    create_packetizer(i);
}

void
vobsub_reader_c::parse_headers() {
  string language, line;
  vobsub_track_c *track  = NULL;
  int64_t line_no        = 0;
  int64_t last_timestamp = 0;
  bool sort_required     = false;
  num_indices            = 0;
  indices_processed      = 0;

  while (1) {
    if (!idx_file->getline2(line))
      break;
    line_no++;

    if ((line.length() == 0) || (line[0] == '#'))
      continue;

    const char *sline = line.c_str();

    if (!strncasecmp(sline, "id:", 3)) {
      if (line.length() >= 6) {
        language        = sline[4];
        language       += sline[5];
        int lang_index  = map_to_iso639_2_code(language.c_str());
        if (-1 != lang_index)
          language = iso639_languages[lang_index].iso639_2_code;
        else
          language = "";
      } else
        language = "";

      if (NULL != track) {
        if (track->entries.empty())
          delete track;
        else {
          tracks.push_back(track);
          if (sort_required) {
            mxverb(2, PFX "Sorting track %u\n", (unsigned int)tracks.size());
            stable_sort(track->entries.begin(), track->entries.end());
          }
        }
      }
      track          = new vobsub_track_c(language);
      last_timestamp = 0;
      sort_required  = false;
      continue;
    }

    if (!strncasecmp(sline, "alt:", 4) || !strncasecmp(sline, "langidx:", 8))
      continue;

    if (starts_with_case(line, "delay:")) {
      line.erase(0, 6);
      strip(line);

      int64_t timestamp;
      if (!parse_timecode(line, timestamp, true))
        mxerror(PFX "'%s', line " LLD ": The 'delay' timestamp could not be parsed.\n", ti.fname.c_str(), line_no);
      delay = timestamp;
    }

    if ((7 == version) && starts_with_case(line, "timestamp:")) {
      if (NULL == track)
        mxerror(PFX ".idx file does not contain an 'id: ...' line to indicate the language.\n");

      strip(line);
      shrink_whitespace(line);
      vector<string> parts = split(line.c_str(), " ");

      if ((4 != parts.size()) || (13 > parts[1].length()) || (downcase(parts[2]) != "filepos:")) {
        mxwarn(PFX "'%s', line " LLD ": The line seems to be a subtitle entry but the format couldn't be recognized. This entry will be skipped.\n",
               ti.fname.c_str(), line_no);
        continue;
      }

      int idx         = 0;
      sline           = parts[3].c_str();
      int64_t filepos = hexvalue(sline[idx]);
      idx++;
      while ((0 != sline[idx]) && ishexdigit(sline[idx])) {
        filepos = (filepos << 4) + hexvalue(sline[idx]);
        idx++;
      }

      parts[1].erase(parts[1].length() - 1);
      int factor = 1;
      if ('-' == parts[1][0]) {
        factor = -1;
        parts[1].erase(0, 1);
      }

      int64_t timestamp;
      if (!parse_timecode(parts[1], timestamp)) {
        mxwarn(PFX "'%s', line " LLD ": The line seems to be a subtitle entry but the format couldn't be recognized. This entry will be skipped.\n",
               ti.fname.c_str(), line_no);
        continue;
      }

      vobsub_entry_c entry;
      entry.position  = filepos;
      entry.timestamp = timestamp * factor + delay;

      if (0 > entry.timestamp) {
        mxwarn(PFX "'%s', line " LLD ": The line seems to be a subtitle entry but the timecode was negative even after adding the track "
               "delay. Negative timecodes are not supported in Matroska. This entry will be skipped.\n", ti.fname.c_str(), line_no);
        continue;
      }

      track->entries.push_back(entry);

      if ((entry.timestamp < last_timestamp) &&
          demuxing_requested('s', tracks.size())) {
        mxwarn(PFX "'%s', line " LLD ": The current timestamp (" FMT_TIMECODE ") is smaller than the last one (" FMT_TIMECODE"). mkvmerge "
               "will sort the entries according to their timestamps. This might result in the wrong order for some subtitle entries. If "
               "this is the case then you have to fix the .idx file manually.\n",
               ti.fname.c_str(), line_no, ARG_TIMECODE(entry.timestamp), ARG_TIMECODE(last_timestamp));
        sort_required = true;
      }
      last_timestamp = entry.timestamp;

      continue;
    }

    idx_data += line;
    idx_data += "\n";
  }

  if (NULL != track) {
    if (track->entries.size() == 0)
      delete track;
    else {
      tracks.push_back(track);
      if (sort_required) {
        mxverb(2, PFX "Sorting track %u\n", (unsigned int)tracks.size());
        stable_sort(track->entries.begin(), track->entries.end());
      }
    }
  }

  if (!identifying && (1 < verbose)) {
    int i, k, tsize = tracks.size();
    for (i = 0; i < tsize; i++) {
      mxinfo("vobsub_reader: Track number %u\n", i);
      for (k = 0; k < tracks[i]->entries.size(); k++)
        mxinfo("vobsub_reader:  %04u position: %12" PRId64 " (0x%04x%08x), timecode: %12" PRId64 " (" FMT_TIMECODE ")\n",
               k, tracks[i]->entries[k].position, (uint32_t)(tracks[i]->entries[k].position >> 32), (uint32_t)(tracks[i]->entries[k].position & 0xffffffff),
               tracks[i]->entries[k].timestamp / 1000000, ARG_TIMECODE_NS(tracks[i]->entries[k].timestamp));
    }
  }
}

#define deliver() deliver_packet(dst_buf, dst_size, timecode, duration, PTZR(track->ptzr));
int
vobsub_reader_c::deliver_packet(unsigned char *buf,
                                int size,
                                int64_t timecode,
                                int64_t default_duration,
                                generic_packetizer_c *ptzr) {
  int64_t duration;

  if ((NULL == buf) || (0 == size)) {
    safefree(buf);
    return -1;
  }

  duration = spu_extract_duration(buf, size, timecode);
  if (1 == -duration) {
    mxverb(2, PFX "Could not extract the duration for a SPU packet in track " LLD " of '%s' (timecode: " FMT_TIMECODE ").\n", ti.id, ti.fname.c_str(), ARG_TIMECODE(timecode));
    duration = default_duration;
  }

  if (2 != -duration)
    ptzr->process(new packet_t(new memory_c(buf, size, true), timecode, duration));
  else
    safefree(buf);

  return -1;
}

// Adopted from mplayer's vobsub.c
int
vobsub_reader_c::extract_one_spu_packet(int64_t track_id) {
  uint32_t len, idx, mpeg_version;
  int c, packet_aid;
  /* Goto start of a packet, it starts with 0x000001?? */
  const unsigned char wanted[] = { 0, 0, 1 };
  unsigned char buf[5];

  vobsub_track_c *track        = tracks[track_id];
  int64_t timecode             = track->entries[track->idx].timestamp;
  int64_t duration             = track->entries[track->idx].duration;
  int64_t extraction_start_pos = track->entries[track->idx].position;
  int64_t extraction_end_pos   = track->idx >= track->entries.size() ? sub_file->get_size() : track->entries[track->idx + 1].position;

  int64_t pts                  = 0;
  unsigned char *dst_buf       = NULL;
  uint32_t dst_size            = 0;
  uint32_t packet_size         = 0;
  int spu_len                  = -1;

  sub_file->setFilePointer(extraction_start_pos);
  track->packet_num++;

  while (1) {
    if ((spu_len >= 0) && ((dst_size >= spu_len) || (sub_file->getFilePointer() >= extraction_end_pos))) {
      if (dst_size != spu_len)
        mxverb(3, "r_vobsub.cpp: stddeliver spu_len %d dst_size %u curpos " LLD " endpos " LLD "\n", spu_len, dst_size, sub_file->getFilePointer(), extraction_end_pos);
      return deliver();
    }
    if (sub_file->read(buf, 4) != 4)
      return deliver();
    while (memcmp(buf, wanted, sizeof(wanted)) != 0) {
      c = sub_file->getch();
      if (0 > c)
        return deliver();
      memmove(buf, buf + 1, 3);
      buf[3] = c;
    }
    switch (buf[3]) {
      case 0xb9:                // System End Code
        return deliver();
        break;

      case 0xba:                // Packet start code
        c = sub_file->getch();
        if (0 > c)
          return deliver();
        if ((c & 0xc0) == 0x40)
          mpeg_version = 4;
        else if ((c & 0xf0) == 0x20)
          mpeg_version = 2;
        else {
          if (!track->mpeg_version_warning_printed) {
            mxwarn(PFX "Unsupported MPEG mpeg_version: 0x%02x in packet " LLD "for track " LLD " for timecode " FMT_TIMECODE ", assuming "
                   "MPEG2. No further warnings will be printed for this track.\n", c, track->packet_num, track_id, ARG_TIMECODE_NS(timecode));
            track->mpeg_version_warning_printed = true;
          }
          mpeg_version = 2;
        }

        if (4 == mpeg_version) {
          if (!sub_file->setFilePointer2(9, seek_current))
            return deliver();
        } else if (2 == mpeg_version) {
          if (!sub_file->setFilePointer2(7, seek_current))
            return deliver();
        } else
          abort();
        break;

      case 0xbd:                // packet
        if (sub_file->read(buf, 2) != 2)
          return deliver();

        len = buf[0] << 8 | buf[1];
        idx = sub_file->getFilePointer() - extraction_start_pos;
        c   = sub_file->getch();

        if (0 > c)
          return deliver();
        if ((c & 0xC0) == 0x40) { // skip STD scale & size
          if (sub_file->getch() < 0)
            return deliver();
          c = sub_file->getch();
          if (0 > c)
            return deliver();
        }
        if ((c & 0xf0) == 0x20) // System-1 stream timestamp
          abort();
        else if ((c & 0xf0) == 0x30)
          abort();
        else if ((c & 0xc0) == 0x80) { // System-2 (.VOB) stream
          uint32_t pts_flags, hdrlen, dataidx;
          c = sub_file->getch();
          if (0 > c)
            return deliver();

          pts_flags = c;
          c         = sub_file->getch();
          if (0 > c)
            return deliver();

          hdrlen  = c;
          dataidx = sub_file->getFilePointer() - extraction_start_pos + hdrlen;
          if (dataidx > idx + len) {
            mxwarn(PFX "Invalid header length: %d (total length: %d, idx: %d, dataidx: %d)\n", hdrlen, len, idx, dataidx);
            return deliver();
          }

          if ((pts_flags & 0xc0) == 0x80) {
            if (sub_file->read(buf, 5) != 5)
              return deliver();
            if (!(((buf[0] & 0xf0) == 0x20) && (buf[0] & 1) && (buf[2] & 1) && (buf[4] & 1))) {
              mxwarn(PFX "PTS error: 0x%02x %02x%02x %02x%02x \n", buf[0], buf[1], buf[2], buf[3], buf[4]);
              pts = 0;
            } else
              pts = ((int64_t)((buf[0] & 0x0e) << 29 | buf[1] << 22 | (buf[2] & 0xfe) << 14 | buf[3] << 7 | (buf[4] >> 1))) * 100000 / 9;
          }

          sub_file->setFilePointer2(dataidx + extraction_start_pos, seek_beginning);
          packet_aid = sub_file->getch();
          if (0 > packet_aid) {
            mxwarn(PFX "Bogus aid %d\n", packet_aid);
            return deliver();
          }

          packet_size = len - ((unsigned int)sub_file->getFilePointer() - extraction_start_pos - idx);
          if (-1 == track->aid)
            track->aid = packet_aid;
          else if (track->aid != packet_aid) {
            // The packet does not belong to the current subtitle stream.
            mxverb(3, PFX "skipping sub packet with aid %d (wanted aid: %d) with size %d at " LLD "\n", packet_aid, track->aid,
                   packet_size, sub_file->getFilePointer() - extraction_start_pos);
            sub_file->skip(packet_size);
            idx = len;
            break;
          }
          dst_buf = (unsigned char *)saferealloc(dst_buf, dst_size + packet_size);
          mxverb(3, PFX "sub packet data: aid: %d, pts: " FMT_TIMECODE ", packet_size: %u\n", track->aid, ARG_TIMECODE_NS(pts), packet_size);
          if (sub_file->read(&dst_buf[dst_size], packet_size) != packet_size) {
            mxwarn(PFX "sub_file->read failure");
            return deliver();
          }
          if (-1 == spu_len)
            spu_len = get_uint16_be(dst_buf);

          dst_size        += packet_size;
          track->spu_size += packet_size;
          track->overhead += sub_file->getFilePointer() - extraction_start_pos - packet_size;

          idx              = len;
        }
        break;

      case 0xbe:                // Padding
        if (sub_file->read(buf, 2) != 2)
          return deliver();
        len = buf[0] << 8 | buf[1];
        if ((0 < len) && !sub_file->setFilePointer2(len, seek_current))
          return deliver();
        break;

      default:
        if ((0xc0 <= buf[3]) && (buf[3] < 0xf0)) {
          // MPEG audio or video
          if (sub_file->read(buf, 2) != 2)
            return deliver();
          len = (buf[0] << 8) | buf[1];
          if ((0 < len) && !sub_file->setFilePointer2(len, seek_current))
            return deliver();

        } else {
          mxwarn(PFX "unknown header 0x%02X%02X%02X%02X\n", buf[0], buf[1], buf[2], buf[3]);
          return deliver();
        }
    }
  }

  return deliver();
}

file_status_e
vobsub_reader_c::read(generic_packetizer_c *ptzr,
                      bool force) {
  vobsub_track_c *track = NULL;
  uint32_t id;

  for (id = 0; id < tracks.size(); ++id)
    if ((-1 != tracks[id]->ptzr) && (PTZR(tracks[id]->ptzr) == ptzr)) {
      track = tracks[id];
      break;
    }

  if (!track || (track->idx >= track->entries.size()))
    return FILE_STATUS_DONE;

  extract_one_spu_packet(id);
  track->idx++;
  indices_processed++;

  if (track->idx >= track->entries.size()) {
    flush_packetizers();
    return FILE_STATUS_DONE;
  }

  return FILE_STATUS_MOREDATA;
}

int
vobsub_reader_c::get_progress() {
  return 100 * indices_processed / num_indices;
}

void
vobsub_reader_c::identify() {
  vector<string> verbose_info;
  int i;

  id_result_container("VobSub");

  for (i = 0; i < tracks.size(); i++) {
    verbose_info.clear();

    if (!tracks[i]->language.empty())
      verbose_info.push_back(string("language:") + tracks[i]->language);

    id_result_track(i, ID_RESULT_TRACK_SUBTITLES, "VobSub", verbose_info);
  }
}

void
vobsub_reader_c::flush_packetizers() {
  uint32_t i;

  for (i = 0; i < tracks.size(); i++)
    if (tracks[i]->ptzr != -1)
      PTZR(tracks[i]->ptzr)->flush();
}

void
vobsub_reader_c::add_available_track_ids() {
  int i;

  for (i = 0; i < tracks.size(); i++)
    available_track_ids.push_back(i);
}
