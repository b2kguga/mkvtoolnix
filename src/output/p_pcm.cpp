/*
  mkvmerge -- utility for splicing together matroska files
      from component media subtypes

  p_pcm.cpp

  Written by Moritz Bunkus <moritz@bunkus.org>

  Distributed under the GPL
  see the file COPYING for details
  or visit http://www.gnu.org/copyleft/gpl.html
*/

/*!
    \file
    \version $Id$
    \brief PCM output module
    \author Moritz Bunkus <moritz@bunkus.org>
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "mkvmerge.h"
#include "common.h"
#include "pr_generic.h"
#include "p_pcm.h"
#include "matroska.h"

using namespace libmatroska;

pcm_packetizer_c::pcm_packetizer_c(generic_reader_c *nreader,
                                   unsigned long nsamples_per_sec,
                                   int nchannels, int nbits_per_sample,
                                   track_info_c *nti, bool nbig_endian)
  throw (error_c):
  generic_packetizer_c(nreader, nti) {
  packetno = 0;
  bps = nchannels * nbits_per_sample * nsamples_per_sec / 8;
  tempbuf = (unsigned char *)safemalloc(bps + 128);
  tempbuf_size = bps;
  samples_per_sec = nsamples_per_sec;
  channels = nchannels;
  bits_per_sample = nbits_per_sample;
  bytes_output = 0;
  remaining_sync = 0;
  big_endian = nbig_endian;

  set_track_type(track_audio);
  if (use_durations)
    set_track_default_duration_ns((int64_t)(1000000000.0 * ti->async.linear /
                                            pcm_interleave));
}

pcm_packetizer_c::~pcm_packetizer_c() {
  if (tempbuf != NULL)
    safefree(tempbuf);
}

void pcm_packetizer_c::set_headers() {
  if (big_endian)
    set_codec_id(MKV_A_PCM_BE);
  else
    set_codec_id(MKV_A_PCM);
  set_audio_sampling_freq((float)samples_per_sec);
  set_audio_channels(channels);
  set_audio_bit_depth(bits_per_sample);

  generic_packetizer_c::set_headers();
}

int pcm_packetizer_c::process(unsigned char *buf, int size,
                              int64_t, int64_t, int64_t, int64_t) {
  int i, bytes_per_packet, remaining_bytes, complete_packets;
  unsigned char *new_buf;

  debug_enter("pcm_packetizer_c::process");

  if (size > tempbuf_size) {
    tempbuf = (unsigned char *)saferealloc(tempbuf, size + 128);
    tempbuf_size = size;
  }

  new_buf = buf;

  if (initial_displacement != 0) {
    if (initial_displacement > 0) {
      // Add silence.
      int pad_size;

      pad_size = bps * initial_displacement / 1000;
      new_buf = (unsigned char *)safemalloc(size + pad_size);
      memset(new_buf, 0, pad_size);
      memcpy(&new_buf[pad_size], buf, size);
      size += pad_size;
    } else
      // Skip bytes.
      remaining_sync = -1 * bps * initial_displacement / 1000;
    initial_displacement = 0;
  }

  if (remaining_sync > 0) {
    if (remaining_sync > size) {
      remaining_sync -= size;
      debug_leave("pcm_packetizer_c::process");
      return EMOREDATA;
    }
    memmove(buf, &buf[remaining_sync], size - remaining_sync);
    size -= remaining_sync;
    remaining_sync = 0;
  }

  bytes_per_packet = bps / pcm_interleave;
  complete_packets = size / bytes_per_packet;
  remaining_bytes = size % bytes_per_packet;

  for (i = 0; i < complete_packets; i++) {
    add_packet(new_buf + i * bytes_per_packet, bytes_per_packet,
               (int64_t)((bytes_output * 1000 / bps) * ti->async.linear),
               (int64_t)(bytes_per_packet * 1000.0 * ti->async.linear / bps));
    bytes_output += bytes_per_packet;
    packetno++;
  }
  if (remaining_bytes != 0) {
    add_packet(new_buf + complete_packets * bytes_per_packet, remaining_bytes,
               (int64_t)((bytes_output * 1000 / bps) * ti->async.linear),
               (int64_t)(remaining_bytes * 1000.0 * ti->async.linear / bps));
    bytes_output += remaining_bytes;
    packetno++;
  }

  if (new_buf != buf)
    safefree(new_buf);

  debug_leave("pcm_packetizer_c::process");

  return EMOREDATA;
}

void pcm_packetizer_c::dump_debug_info() {
  mxdebug("pcm_packetizer_c: queue: %d\n", packet_queue.size());
}
