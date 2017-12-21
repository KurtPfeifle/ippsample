/*
 * Temp file utilities for CUPS.
 *
 * Copyright 2007-2014 by Apple Inc.
 * Copyright 1997-2006 by Easy Software Products.
 *
 * Licensed under Apache License v2.0.  See the file "LICENSE" for more information.
 */

/*
 * Include necessary headers...
 */

#include "cups-private.h"
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(WIN32) || defined(__EMX__)
#  include <io.h>
#else
#  include <unistd.h>
#endif /* WIN32 || __EMX__ */


/*
 * 'cupsTempFd()' - Creates a temporary file.
 *
 * The temporary filename is returned in the filename buffer.
 * The temporary file is opened for reading and writing.
 */

int					/* O - New file descriptor or -1 on error */
cupsTempFd(char *filename,		/* I - Pointer to buffer */
           int  len)			/* I - Size of buffer */
{
  int		fd;			/* File descriptor for temp file */
  int		tries;			/* Number of tries */
  const char	*tmpdir;		/* TMPDIR environment var */
#ifdef WIN32
  char		tmppath[1024];		/* Windows temporary directory */
  DWORD		curtime;		/* Current time */
#else
  struct timeval curtime;		/* Current time */
#endif /* WIN32 */


 /*
  * See if TMPDIR is defined...
  */

#ifdef WIN32
  if ((tmpdir = getenv("TEMP")) == NULL)
  {
    GetTempPath(sizeof(tmppath), tmppath);
    tmpdir = tmppath;
  }
#else
 /*
  * Previously we put root temporary files in the default CUPS temporary
  * directory under /var/spool/cups.  However, since the scheduler cleans
  * out temporary files there and runs independently of the user apps, we
  * don't want to use it unless specifically told to by cupsd.
  */

  if ((tmpdir = getenv("TMPDIR")) == NULL)
#  if defined(__APPLE__) && !TARGET_OS_IOS
    tmpdir = "/private/tmp";		/* /tmp is a symlink to /private/tmp */
#  else
    tmpdir = "/tmp";
#  endif /* __APPLE__  && !TARGET_OS_IOS */
#endif /* WIN32 */

 /*
  * Make the temporary name using the specified directory...
  */

  tries = 0;

  do
  {
#ifdef WIN32
   /*
    * Get the current time of day...
    */

    curtime =  GetTickCount() + tries;

   /*
    * Format a string using the hex time values...
    */

    snprintf(filename, (size_t)len - 1, "%s/%05lx%08lx", tmpdir, GetCurrentProcessId(), curtime);
#else
   /*
    * Get the current time of day...
    */

    gettimeofday(&curtime, NULL);

   /*
    * Format a string using the hex time values...
    */

    snprintf(filename, (size_t)len - 1, "%s/%05x%08x", tmpdir, (unsigned)getpid(), (unsigned)(curtime.tv_sec + curtime.tv_usec + tries));
#endif /* WIN32 */

   /*
    * Open the file in "exclusive" mode, making sure that we don't
    * stomp on an existing file or someone's symlink crack...
    */

#ifdef WIN32
    fd = open(filename, _O_CREAT | _O_RDWR | _O_TRUNC | _O_BINARY,
              _S_IREAD | _S_IWRITE);
#elif defined(O_NOFOLLOW)
    fd = open(filename, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#else
    fd = open(filename, O_RDWR | O_CREAT | O_EXCL, 0600);
#endif /* WIN32 */

    if (fd < 0 && errno != EEXIST)
      break;

    tries ++;
  }
  while (fd < 0 && tries < 1000);

 /*
  * Return the file descriptor...
  */

  return (fd);
}


/*
 * 'cupsTempFile()' - Generates a temporary filename.
 *
 * The temporary filename is returned in the filename buffer.
 * This function is deprecated and will no longer generate a temporary
 * filename - use @link cupsTempFd@ or @link cupsTempFile2@ instead.
 *
 * @deprecated@
 */

char *					/* O - Filename or @code NULL@ on error */
cupsTempFile(char *filename,		/* I - Pointer to buffer */
             int  len)			/* I - Size of buffer */
{
  (void)len;

  if (filename)
    *filename = '\0';

  return (NULL);
}


/*
 * 'cupsTempFile2()' - Creates a temporary CUPS file.
 *
 * The temporary filename is returned in the filename buffer.
 * The temporary file is opened for writing.
 *
 * @since CUPS 1.2/macOS 10.5@
 */

cups_file_t *				/* O - CUPS file or @code NULL@ on error */
cupsTempFile2(char *filename,		/* I - Pointer to buffer */
              int  len)			/* I - Size of buffer */
{
  cups_file_t	*file;			/* CUPS file */
  int		fd;			/* File descriptor */


  if ((fd = cupsTempFd(filename, len)) < 0)
    return (NULL);
  else if ((file = cupsFileOpenFd(fd, "w")) == NULL)
  {
    close(fd);
    unlink(filename);
    return (NULL);
  }
  else
    return (file);
}
