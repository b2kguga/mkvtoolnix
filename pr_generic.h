/*
  mkvmerge -- utility for splicing together matroska files
      from component media subtypes

  pr_generic.h

  Written by Moritz Bunkus <moritz@bunkus.org>

  Distributed under the GPL
  see the file COPYING for details
  or visit http://www.gnu.org/copyleft/gpl.html
*/

/*!
    \file
    \version \$Id: pr_generic.h,v 1.1 2003/02/16 17:04:39 mosu Exp $
    \brief class definition for the generic reader and packetizer
    \author Moritz Bunkus         <moritz @ bunkus.org>
*/

#ifndef __PR_GENERIC_H
#define __PR_GENERIC_H

#include <stdio.h>

#include "common.h"
#include "KaxTracks.h"

extern LIBMATROSKA_NAMESPACE::KaxTracks *kax_tracks;
extern LIBMATROSKA_NAMESPACE::KaxTrackEntry *kax_last_entry;
extern int track_number;

struct packet_t;

typedef class generic_packetizer_c {
  protected:
    int serialno;
  public:
    LIBMATROSKA_NAMESPACE::KaxTrackEntry *track_entry;

    generic_packetizer_c() { track_entry = NULL; };
    virtual ~generic_packetizer_c() {};
    virtual int       packet_available() = 0;
    virtual packet_t *get_packet() = 0;
    virtual void      set_header() = 0;
    virtual stamp_t   get_smallest_timestamp() = 0;
} generic_packetizer_c;
 
typedef class generic_reader_c {
//  protected:
//    vorbis_comment *chapter_info;
  public:
    generic_reader_c() {};
    virtual ~generic_reader_c() {};
    virtual int              read() = 0;
    virtual packet_t        *get_packet() = 0;
    virtual int              display_priority() = 0;
    virtual void             display_progress() = 0;
//    virtual void             set_chapter_info(vorbis_comment *info);
} generic_reader_c;

typedef struct packet_t {
  char                 *data;
  int                   length;
  u_int64_t             timestamp;
  int                   is_key;
  generic_packetizer_c *source;
} packet_t;

#endif  // __PR_GENERIC_H
