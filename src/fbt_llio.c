/**
 * @file fbt_llio.c
 * Low-level I/O functions for use in system call authorization and
 * instrumentation functions. Some functions that trigger system calls, like
 * malloc (mmap syscall) or printf (write syscall) are not reentrant.
 * This means that these functions cannot be used in system call authorization
 * and instrumentation functions, at least not for the corresponding syscalls.
 *
 * Copyright (c) 2011 ETH Zurich
 * @author Mathias Payer <mathias.payer@nebelwelt.net>
 *
 * $Date: 2011-03-23 10:26:53 +0100 (Wed, 23 Mar 2011) $
 * $LastChangedDate: 2011-03-23 10:26:53 +0100 (Wed, 23 Mar 2011) $
 * $LastChangedBy: payerm $
 * $Revision: 443 $
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */
#include "fbt_llio.h"

#if !defined(DEBUG)
#include <stdarg.h>
#endif

#include "fbt_libc.h"

#if !defined(DEBUG)
/**
 * Write a formatted string to the file descriptor fd (might use a buffer). Used
 * only during debugging.
 * @param fd File descriptor that is written to.
 * @param format format string that is interpreted (including all varargs).
 * @param ap List of var args.
 * @return Number of bytes written.
 */
int fllprintfva(int fd, const char* format, va_list ap);
#endif

/** minimum between two values */
#define MIN(a,b) ((a)>(b)?(b):(a))

/** absolute value of a signed int32 value */
#define ABS(a) ((a>>31)^(a+(a>>31)))

/**
 * small buffer size for the low-level printing functions.
 * The output string cannot be longer than BUFSIZE_S -1. Used for decimal and
 * hexadecimal numbers.
 */
#define BUFSIZE_S 16

/**
 * large buffer size for the low-level printing functions.
 * The output string cannot be longer than BUFSIZE_L -1. Used for formatted
 * strings.
 */
#define BUFSIZE_L 512

int fllwrite(int fd, const char* str)
{
  int written = 0;
  int length = fbt_strnlen(str, 0);
  while (written < length) {
    int retval;
    fbt_write(fd, str + written, length - written, retval);
    if (retval < 0) {
      fbt_suicide(255);
      /*
      // handle errors and interrupted writes
      if (errno != EINTR) {
        break;
        }*/
    } else {
      written += retval;
    }
  }
  return written;
}

int fllprintf(int fd, const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  int ret = fllprintfva(fd, format, ap);
  va_end(ap);
  return ret;
}

#define FDBUFSIZE (BUFSIZE_L+1)
struct llbuf {
  int fd;
  char buffer[FDBUFSIZE];
};
#define NRFDBUFS 4
static struct llbuf buffers[NRFDBUFS] = {
  {.fd=-1}, {.fd=-1}, {.fd=-1}, {.fd=-1}
};

int fllprintfva(int fd, const char* format, va_list app)
{
  char buf[BUFSIZE_L+1];

  int bi = 0;     // index in the output string
  int fi = 0;     // index in the format string
  int end = 0;
  while ((bi < (BUFSIZE_L)) && !end) {
    // parse format string and write contents to buffer
    switch (format[fi]) {
    case '%':
      fi++;
      unsigned char len = 0;
      /* limit len of output/datatype? */
      if (format[fi]=='.') {
        len=format[++fi]-0x30;
        if (len>BUFSIZE_S) len=0;
        fi++;
        if (format[fi]-0x30>=0 && format[fi]-0x30<=9) {
          /* two digit number */
          len*=10;
          len+=format[fi]-0x30;
          fi++;
        }
      }
      char *pointer;
      char revbuf[BUFSIZE_S+1];
      int i, val, length;
      unsigned int abs_d;
      for (i=0; i<BUFSIZE_S+1; i++) revbuf[i]=0x0;
      i=BUFSIZE_S; // initialize to back of buffer;
      switch (format[fi]) {
      case '%':
        buf[bi++] = '%';
        break;
      case 'd':
      case 'i':
        val = va_arg(app, int);
        abs_d = ABS(val);
        while ((abs_d > 0) && (i > 0)) {
          revbuf[--i] = (char) (0x30 + abs_d % 10);
          abs_d /= 10;
        }
        if (val == 0) revbuf[--i] = '0';
        else if (val < 0) revbuf[--i] = '-';
        // if enough space in large buffer copy all
        length = MIN(BUFSIZE_L - bi, BUFSIZE_S - i);
        fbt_strncpy(&buf[bi], &revbuf[i], length);
        bi += length;
        break;
      case 'p':
        buf[bi++]='0';
        buf[bi++]='x';
        if (len<2) len=10;  /* ensure that len is large enough */
        if (len!=0) len-=2;  /* make len smaller (preceding '0x') */
      case 'x':
        abs_d =  va_arg(app, unsigned int);
        while ((abs_d > 0) && (i > 0)) {
          if ((abs_d&0xf) < 0xa) {
            revbuf[--i] = 0x30 + (abs_d&0xf);
          } else {
            revbuf[--i] = 0x57 + (abs_d&0xf);
          }
          abs_d /= 16;
        }
        if (abs_d == 0) revbuf[--i] = '0';
        /* fill leading 0s */
        if (len!=0) while (len>(BUFSIZE_S-i)) revbuf[--i]='0'; 
        length = MIN(BUFSIZE_L - bi, BUFSIZE_S - i);
        if (length>len && len!=0) {
          /* ensure that we stay in buffer */
          i=BUFSIZE_S-len;
          length = len;
        }
        fbt_strncpy(&buf[bi], &revbuf[i], length);
        bi += length;
        break;
      case 's':
        pointer = va_arg(app, char*);
        /* ensure that we stay in buffer */
        int slen = fbt_strnlen(pointer, BUFSIZE_L - bi);
        if (slen>len && len!=0) slen=len;
        fbt_strncpy(&buf[bi], pointer, slen);
        bi += slen;
        break;
      case '\0':
        /* end of format string, break out of while loop */
        buf[bi]=format[fi];
        end = 1;
        break;
      default:
        /* we have a % but no valid conversion specifier
           -> we copy the '%' and the following character verbatim */
        buf[bi] = '%';
        bi++;
        buf[bi] = format[fi];
        bi++;
      }
      break;
    case '\0':
      /* end of format string, break out of while loop */
      buf[bi] = '\0';
      end = 1;
      break;
    default:
      buf[bi] = format[fi];
      bi++;
    }
    fi++;
  }
  buf[BUFSIZE_L]=0x0;  /* guard */

  int bufpos = -1;
  /* should we buffer the current string? */
  for (fi=0; fi<NRFDBUFS; fi++) if (buffers[fi].fd==fd) {
      bufpos = fi;
      break;
    }

  //return fllwrite(fd, buf);
  // newline -> flush
  // buffer too small? -> flush
  // otherwise add to buf
  int strlen = fbt_strnlen(buf,BUFSIZE_L);
  if (bufpos!=-1 && (fbt_strnlen(buffers[bufpos].buffer,BUFSIZE_L) + strlen >=
                     FDBUFSIZE)) {
    /* write stuff to fd */
    if (bufpos!=-1) {
      fllwrite(fd, buffers[bufpos].buffer);
      buffers[bufpos].fd=-1;  /* free buffer, all flushed */
    }
    fllwrite(fd, buf);
  } else {
    /* let's buffer */
    if (bufpos==-1) {
      /* we don't have a buffer associated with this fd (yet) */
      for (fi=0; fi<NRFDBUFS; fi++) if (buffers[fi].fd==-1) {
          bufpos = fi;
          break;
        }
      if (bufpos==-1) {
        /* we have no empty seats, flush first entry */
        fllwrite(buffers[0].fd, buffers[0].buffer);
        bufpos=0;
      }
      buffers[bufpos].buffer[0]='\0';
      buffers[bufpos].fd=fd;
    }
    /* add stuff to buffer (we know that buf does not overflow our buffer) */
    fbt_strncpy(buffers[bufpos].buffer + fbt_strnlen(buffers[bufpos].buffer,
                                                     FDBUFSIZE), buf, strlen);
  }
  return strlen;
}

void ffflush()
{
  int i;
  for (i=0; i<NRFDBUFS; i++) {
    if (buffers[i].fd!=-1) {
      fllwrite(buffers[i].fd, buffers[i].buffer);
      buffers[i].fd=-1;
    }
  }
}
