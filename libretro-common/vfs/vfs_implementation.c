/* Copyright  (C) 2010-2017 The RetroArch team
*
* ---------------------------------------------------------------------------------------
* The following license statement only applies to this file (vfs_implementation.c).
* ---------------------------------------------------------------------------------------
*
* Permission is hereby granted, free of charge,
* to any person obtaining a copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation the rights to
* use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
* and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
* INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(_WIN32)
#  ifdef _MSC_VER
#    define setmode _setmode
#  endif
#  ifdef _XBOX
#    include <xtl.h>
#    define INVALID_FILE_ATTRIBUTES -1
#  else
#    include <io.h>
#    include <fcntl.h>
#    include <direct.h>
#    include <windows.h>
#  endif
#else
#  if defined(PSP)
#    include <pspiofilemgr.h>
#  endif
#  include <sys/types.h>
#  include <sys/stat.h>
#  if !defined(VITA)
#  include <dirent.h>
#  endif
#  include <unistd.h>
#endif

#ifdef __CELLOS_LV2__
#include <cell/cell_fs.h>
#define O_RDONLY CELL_FS_O_RDONLY
#define O_WRONLY CELL_FS_O_WRONLY
#define O_CREAT CELL_FS_O_CREAT
#define O_TRUNC CELL_FS_O_TRUNC
#define O_RDWR CELL_FS_O_RDWR
#else
#include <fcntl.h>
#endif

#include <streams/file_stream.h>

/* Assume W-functions do not work below Win2K and Xbox platforms */
#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0500 || defined(_XBOX)

#ifndef LEGACY_WIN32
#define LEGACY_WIN32
#endif

#endif

#if !defined(_WIN32) || defined(LEGACY_WIN32)
#define MODE_STR_READ "r"
#define MODE_STR_READ_UNBUF "rb"
#define MODE_STR_WRITE_UNBUF "wb"
#define MODE_STR_WRITE_PLUS "w+"
#else
#define MODE_STR_READ L"r"
#define MODE_STR_READ_UNBUF L"rb"
#define MODE_STR_WRITE_UNBUF L"wb"
#define MODE_STR_WRITE_PLUS L"w+"
#endif

#include <vfs/vfs_implementation.h>
#include <string/stdstring.h>
#include <memmap.h>
#include <retro_miscellaneous.h>
#include <encodings/utf.h>

#define RFILE_HINT_UNBUFFERED (1 << 8)

struct libretro_vfs_implementation_file
{
   int fd;
   unsigned hints;
   int64_t size;
#if defined(HAVE_MMAP)
   uint64_t mappos;
   uint64_t mapsize;
#endif
   char *buf;
   FILE *fp;
#if defined(HAVE_MMAP)
   uint8_t *mapped;
#endif
};

int64_t retro_vfs_file_seek_internal(void *data, int64_t offset, int whence)
{
   struct libretro_vfs_implementation_file *stream = (struct libretro_vfs_implementation_file*)data;
   if (!stream)
      goto error;

   if ((stream->hints & RFILE_HINT_UNBUFFERED) == 0)
      return fseek(stream->fp, (long)offset, whence);

#ifdef HAVE_MMAP
   /* Need to check stream->mapped because this function is
    * called in filestream_open() */
   if (stream->mapped && stream->hints & 
         RETRO_VFS_FILE_ACCESS_HINT_MEMORY_MAP)
   {
      /* fseek() returns error on under/overflow but 
       * allows cursor > EOF for
         read-only file descriptors. */
      switch (whence)
      {
         case SEEK_SET:
            if (offset < 0)
               goto error;

            stream->mappos = offset;
            break;

         case SEEK_CUR:
            if ((offset   < 0 && stream->mappos + offset > stream->mappos) ||
                  (offset > 0 && stream->mappos + offset < stream->mappos))
               goto error;

            stream->mappos += offset;
            break;

         case SEEK_END:
            if (stream->mapsize + offset < stream->mapsize)
               goto error;

            stream->mappos = stream->mapsize + offset;
            break;
      }
      return stream->mappos;
   }
#endif

   if (lseek(stream->fd, offset, whence) < 0)
      goto error;

   return 0;

error:
   return -1;
}

/**
 * retro_vfs_file_open_impl:
 * @path               : path to file
 * @mode               : file mode to use when opening (read/write)
 * @hints              :
 *
 * Opens a file for reading or writing, depending on the requested mode.
 * Returns a pointer to an RFILE if opened successfully, otherwise NULL.
 **/

void *retro_vfs_file_open_impl(const char *path, unsigned mode, unsigned hints)
{
   int            flags    = 0;
#if !defined(_WIN32) || defined(LEGACY_WIN32)
   const char *mode_str    = NULL;
#else
   const wchar_t *mode_str = NULL;
#endif
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)calloc(1, sizeof(*stream));

   if (!stream)
      return NULL;

   (void)flags;

   stream->hints           = hints;

#ifdef HAVE_MMAP
   if (stream->hints & RETRO_VFS_FILE_ACCESS_HINT_MEMORY_MAP && mode == RETRO_VFS_FILE_ACCESS_READ)
      stream->hints |= RFILE_HINT_UNBUFFERED;
   else
#endif
      stream->hints &= ~RETRO_VFS_FILE_ACCESS_HINT_MEMORY_MAP;

   switch (mode)
   {
      case RETRO_VFS_FILE_ACCESS_READ:
         if ((stream->hints & RFILE_HINT_UNBUFFERED) == 0)
            mode_str = MODE_STR_READ_UNBUF;
         /* No "else" here */
         flags    = O_RDONLY;
         break;
      case RETRO_VFS_FILE_ACCESS_WRITE:
         if ((stream->hints & RFILE_HINT_UNBUFFERED) == 0)
            mode_str = MODE_STR_WRITE_UNBUF;
         else
         {
            flags    = O_WRONLY | O_CREAT | O_TRUNC;
#ifndef _WIN32
            flags   |=  S_IRUSR | S_IWUSR;
#endif
         }
         break;
      case RETRO_VFS_FILE_ACCESS_READ_WRITE:
         if ((stream->hints & RFILE_HINT_UNBUFFERED) == 0)
            mode_str = MODE_STR_WRITE_PLUS;
         else
         {
            flags    = O_RDWR;
#ifdef _WIN32
            flags   |= O_BINARY;
#endif
         }
         break;
         /* TODO/FIXME - implement */
      case RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING:
         break;
   }

   if ((stream->hints & RFILE_HINT_UNBUFFERED) == 0 && mode_str)
   {
#if defined(_WIN32) && !defined(_XBOX)
#if defined(LEGACY_WIN32)
      char *path_local    = utf8_to_local_string_alloc(path);
      stream->fp          = fopen(path_local, mode_str);
      if (path_local)
         free(path_local);
#else
      wchar_t * path_wide = utf8_to_utf16_string_alloc(path);
      stream->fp          = _wfopen(path_wide, mode_str);
      if (path_wide)
         free(path_wide);
#endif
#else
      stream->fp = fopen(path, mode_str);
#endif

      if (!stream->fp)
         goto error;

      /* Regarding setvbuf:
       *
       * https://www.freebsd.org/cgi/man.cgi?query=setvbuf&apropos=0&sektion=0&manpath=FreeBSD+11.1-RELEASE&arch=default&format=html
       *
       * If the size argument is not zero but buf is NULL, a buffer of the given size will be allocated immediately, and
       * released on close. This is an extension to ANSI C.
       *
       * Since C89 does not support specifying a null buffer with a non-zero size, we create and track our own buffer for it.
       */
      /* TODO: this is only useful for a few platforms, find which and add ifdef */
      stream->buf = (char*)calloc(1, 0x4000);
      setvbuf(stream->fp, stream->buf, _IOFBF, 0x4000);
   }
   else
   {
#if defined(_WIN32) && !defined(_XBOX)
#if defined(LEGACY_WIN32)
      char *path_local    = utf8_to_local_string_alloc(path);
      stream->fd          = open(path_local, flags, 0);
      if (path_local)
         free(path_local);
#else
      wchar_t * path_wide = utf8_to_utf16_string_alloc(path);
      stream->fd          = _wopen(path_wide, flags, 0);
      if (path_wide)
         free(path_wide);
#endif
#else
      stream->fd = open(path, flags, 0);
#endif

      if (stream->fd == -1)
         goto error;

#ifdef HAVE_MMAP
      if (stream->hints & RETRO_VFS_FILE_ACCESS_HINT_MEMORY_MAP)
      {
         stream->mappos  = 0;
         stream->mapped  = NULL;
         stream->mapsize = retro_vfs_file_seek_internal(stream, 0, SEEK_END);

         if (stream->mapsize == (uint64_t)-1)
            goto error;

         retro_vfs_file_seek_internal(stream, 0, SEEK_SET);

         stream->mapped = (uint8_t*)mmap((void*)0,
               stream->mapsize, PROT_READ,  MAP_SHARED, stream->fd, 0);

         if (stream->mapped == MAP_FAILED)
            stream->hints &= ~RETRO_VFS_FILE_ACCESS_HINT_MEMORY_MAP;
      }
#endif
   }

   retro_vfs_file_seek_internal(stream, 0, SEEK_SET);
   retro_vfs_file_seek_internal(stream, 0, SEEK_END);

   stream->size = retro_vfs_file_tell_impl(stream);

   retro_vfs_file_seek_internal(stream, 0, SEEK_SET);

   return stream;

error:
   retro_vfs_file_close_impl(stream);
   return NULL;
}

int retro_vfs_file_close_impl(void *data)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
   if (!stream)
      return -1;

   if ((stream->hints & RFILE_HINT_UNBUFFERED) == 0)
   {
      if (stream->fp)
         fclose(stream->fp);
   }
   else
   {
#ifdef HAVE_MMAP
      if (stream->hints & RETRO_VFS_FILE_ACCESS_HINT_MEMORY_MAP)
         munmap(stream->mapped, stream->mapsize);
#endif
   }

   if (stream->fd > 0)
      close(stream->fd);
   if (stream->buf)
      free(stream->buf);
   free(stream);

   return 0;
}

int retro_vfs_file_error_impl(void *data)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
   return ferror(stream->fp);
}

int64_t retro_vfs_file_size_impl(void *data)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
   if (!stream)
      return 0;
   return stream->size;
}

int64_t retro_vfs_file_tell_impl(void *data)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
   if (!stream)
      return -1;

   if ((stream->hints & RFILE_HINT_UNBUFFERED) == 0)
      return ftell(stream->fp);

#ifdef HAVE_MMAP
   /* Need to check stream->mapped because this function
    * is called in filestream_open() */
   if (stream->mapped && stream->hints & RETRO_VFS_FILE_ACCESS_HINT_MEMORY_MAP)
      return stream->mappos;
#endif
   if (lseek(stream->fd, 0, SEEK_CUR) < 0)
      return -1;

   return 0;
}

int64_t retro_vfs_file_seek_impl(void *data, int64_t offset, int whence)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
	return retro_vfs_file_seek_internal(stream, offset, whence);
}

int64_t retro_vfs_file_read_impl(void *data, void *s, uint64_t len)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
   if (!stream || !s)
      goto error;

   if ((stream->hints & RFILE_HINT_UNBUFFERED) == 0)
      return fread(s, 1, len, stream->fp);

#ifdef HAVE_MMAP
   if (stream->hints & RETRO_VFS_FILE_ACCESS_HINT_MEMORY_MAP)
   {
      if (stream->mappos > stream->mapsize)
         goto error;

      if (stream->mappos + len > stream->mapsize)
         len = stream->mapsize - stream->mappos;

      memcpy(s, &stream->mapped[stream->mappos], len);
      stream->mappos += len;

      return len;
   }
#endif

   return read(stream->fd, s, len);

error:
   return -1;
}

int64_t retro_vfs_file_write_impl(void *data, const void *s, uint64_t len)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
   if (!stream)
      goto error;

   if ((stream->hints & RFILE_HINT_UNBUFFERED) == 0)
      return fwrite(s, 1, len, stream->fp);

#ifdef HAVE_MMAP
   if (stream->hints & RETRO_VFS_FILE_ACCESS_HINT_MEMORY_MAP)
      goto error;
#endif
   return write(stream->fd, s, len);

error:
   return -1;
}

int retro_vfs_file_flush_impl(void *data)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
   if (!stream)
      return -1;
   return fflush(stream->fp);
}

int retro_vfs_file_delete_impl(const char *path)
{
   return remove(path) == 0;
}

const char *retro_vfs_file_get_path_impl(void *data)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
   /* TODO/FIXME - implement */
   return NULL;
}

int retro_vfs_file_putc(void *data, int c)
{
   struct libretro_vfs_implementation_file *stream = 
      (struct libretro_vfs_implementation_file*)data;
   if (!stream)
      return EOF;

   return fputc(c, stream->fp);
}