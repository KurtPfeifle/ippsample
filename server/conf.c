/*
 * Configuration file support for sample IPP server implementation.
 *
 * Copyright 2015 by Apple Inc.
 *
 * These coded instructions, statements, and computer programs are the
 * property of Apple Inc. and are protected by Federal copyright
 * law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 * which should have been included with this file.  If this file is
 * file is missing or damaged, see the license at "http://www.cups.org/".
 *
 * This file is subject to the Apple OS-Developed Software exception.
 */

#include "ippserver.h"
#include <cups/file.h>
#include <cups/dir.h>
#include <fnmatch.h>


/*
 * Local functions...
 */

static int		compare_printers(server_printer_t *a, server_printer_t *b);
static ipp_t		*get_collection(cups_file_t *fp, const char *filename, int *linenum);
static char		*get_token(cups_file_t *fp, char *buf, int buflen, int *linenum);
static int		load_system(const char *conf);


/*
 * 'serverDNSSDInit()' - Initialize DNS-SD registrations.
 */

void
serverDNSSDInit(void)
{
#ifdef HAVE_DNSSD
  if (DNSServiceCreateConnection(&DNSSDMaster) != kDNSServiceErr_NoError)
  {
    fputs("Error: Unable to initialize Bonjour.\n", stderr);
    exit(1);
  }

#elif defined(HAVE_AVAHI)
  int error;			/* Error code, if any */

  if ((DNSSDMaster = avahi_threaded_poll_new()) == NULL)
  {
    fputs("Error: Unable to initialize Bonjour.\n", stderr);
    exit(1);
  }

  if ((DNSSDClient = avahi_client_new(avahi_threaded_poll_get(DNSSDMaster), AVAHI_CLIENT_NO_FAIL, dnssd_client_cb, NULL, &error)) == NULL)
  {
    fputs("Error: Unable to initialize Bonjour.\n", stderr);
    exit(1);
  }

  avahi_threaded_poll_start(DNSSDMaster);
#endif /* HAVE_DNSSD */
}


/*
 * 'serverFinalizeConfiguration()' - Make final configuration choices.
 */

int					/* O - 1 on success, 0 on failure */
serverFinalizeConfiguration(void)
{
  char			local[1024],	/* Local hostname */
			uri[1024];	/* Printer URI */
  server_printer_t	*printer;	/* Current printer */
  server_listener_t	*lis;		/* First listener */


 /*
  * Default hostname...
  */

  if (!ServerName && httpGetHostname(NULL, local, sizeof(local)))
    ServerName = strdup(local);
  else
    ServerName = strdup("localhost");

#ifdef HAVE_SSL
 /*
  * Setup TLS certificate for server...
  */

  cupsSetServerCredentials(KeychainPath, ServerName, 1);
#endif /* HAVE_SSL */

 /*
  * Default directories...
  */

  if (!DataDirectory)
  {
    char	directory[1024];	/* New directory */
    const char	*tmpdir;		/* Temporary directory */

#ifdef WIN32
    if ((tmpdir = getenv("TEMP")) == NULL)
      tmpdir = "C:/TEMP";
#elif defined(__APPLE__)
    if ((tmpdir = getenv("TMPDIR")) == NULL)
      tmpdir = "/private/tmp";
#else
    if ((tmpdir = getenv("TMPDIR")) == NULL)
      tmpdir = "/tmp";
#endif /* WIN32 */

    snprintf(directory, sizeof(directory), "%s/ippserver.%d", tmpdir, (int)getpid());

    if (mkdir(directory, 0755) && errno != EEXIST)
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Unable to create data directory \"%s\": %s", directory, strerror(errno));
      return (0);
    }

    serverLog(SERVER_LOGLEVEL_INFO, "Using data directory \"%s\".", directory);

    DataDirectory = strdup(directory);
  }

  if (!SpoolDirectory)
  {
    SpoolDirectory = strdup(DataDirectory);

    serverLog(SERVER_LOGLEVEL_INFO, "Using spool directory \"%s\".", DataDirectory);
  }

 /*
  * Initialize Bonjour...
  */

  serverDNSSDInit();

 /*
  * Apply default listeners if none are specified...
  */

  if (!Listeners)
  {
#ifdef WIN32
   /*
    * Windows is almost always used as a single user system, so use a default port
    * number of 8631.
    */

    if (!DefaultPort)
      DefaultPort = 8631;

#else
   /*
    * Use 8000 + UID mod 1000 for the default port number...
    */

    if (!DefaultPort)
      DefaultPort = 8000 + (getuid() % 1000);
#endif /* WIN32 */

    if (!serverCreateListeners(ServerName, DefaultPort))
      return (0);
  }

 /*
  * Save the default printer URI for each configured printer...
  */

  lis = (server_listener_t *)cupsArrayFirst(Listeners);
  for (printer = (server_printer_t *)cupsArrayFirst(Printers); printer; printer = (server_printer_t *)cupsArrayNext(Printers))
  {
#ifdef HAVE_SSL
    httpAssembleURI(HTTP_URI_CODING_ALL, uri, sizeof(uri), Encryption != HTTP_ENCRYPTION_NEVER ? "ipps" : "ipp", NULL, lis->host, lis->port, printer->resource);
#else
    httpAssembleURI(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL, lis->host, lis->port, printer->resource);
#endif /* HAVE_SSL */

    printer->default_uri = strdup(uri);
  }

  return (1);
}


/*
 * 'serverLoadAttributes()' - Load printer attributes from a file.
 *
 * Syntax is based on ipptool format:
 *
 *    ATTR value-tag name value
 *    AUTHTYPE "scheme"
 *    COMMAND "/path/to/command"
 *    DEVICE-URI "uri"
 *    MAKE "manufacturer"
 *    MODEL "model name"
 *    PROXY-USER "username"
 *
 * AUTH schemes are "none" for no authentication or "basic" for HTTP Basic
 * authentication.
 *
 * DEVICE-URI values can be "socket", "ipp", or "ipps" URIs.
 */

ipp_t *					/* O - Attributes */
serverLoadAttributes(
    const char *filename,		/* I - File to load */
    char       **authtype,		/* O - Authentication type, if any */
    char       **command,		/* O - Command to run, if any */
    char       **device_uri,		/* O - Device URI, if any */
    char       **make,			/* O - Manufacturer */
    char       **model,			/* O - Model */
    char       **proxy_user)		/* O - Proxy user, if any */
{
  ipp_t		*attrs;			/* Attributes to return */
  cups_file_t	*fp;			/* File */
  int		linenum = 0;		/* Current line number */
  char		attr[128],		/* Attribute name */
		token[1024],		/* Token from file */
		*tokenptr;		/* Pointer into token */
  ipp_tag_t	value;			/* Current value type */
  ipp_attribute_t *attrptr;		/* Attribute pointer */


  if ((fp = cupsFileOpen(filename, "r")) == NULL)
  {
    serverLog(SERVER_LOGLEVEL_ERROR, "Unable to open \"%s\": %s", filename, strerror(errno));
    return (NULL);
  }

  attrs = ippNew();

  while (get_token(fp, token, sizeof(token), &linenum) != NULL)
  {
    if (!_cups_strcasecmp(token, "ATTR"))
    {
     /*
      * Attribute...
      */

      if (!get_token(fp, token, sizeof(token), &linenum))
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Missing ATTR value tag on line %d of \"%s\".", linenum, filename);
        goto load_error;
      }

      if ((value = ippTagValue(token)) == IPP_TAG_ZERO)
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Bad ATTR value tag \"%s\" on line %d of \"%s\".", token, linenum, filename);
        goto load_error;
      }

      if (!get_token(fp, attr, sizeof(attr), &linenum))
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Missing ATTR name on line %d of \"%s\".", linenum, filename);
        goto load_error;
      }

      if (!get_token(fp, token, sizeof(token), &linenum))
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Missing ATTR value on line %d of \"%s\".", linenum, filename);
        goto load_error;
      }

      attrptr = NULL;

      switch (value)
      {
	case IPP_TAG_BOOLEAN :
	    if (!_cups_strcasecmp(token, "true"))
	      attrptr = ippAddBoolean(attrs, IPP_TAG_PRINTER, attr, 1);
	    else
	      attrptr = ippAddBoolean(attrs, IPP_TAG_PRINTER, attr, (char)atoi(token));
	    break;

	case IPP_TAG_INTEGER :
	case IPP_TAG_ENUM :
	    if (!strchr(token, ','))
	      attrptr = ippAddInteger(attrs, IPP_TAG_PRINTER, value, attr, (int)strtol(token, &tokenptr, 0));
	    else
	    {
	      int	values[100],	/* Values */
			num_values = 1;	/* Number of values */

	      values[0] = (int)strtol(token, &tokenptr, 10);
	      while (tokenptr && *tokenptr &&
		     num_values < (int)(sizeof(values) / sizeof(values[0])))
	      {
		if (*tokenptr == ',')
		  tokenptr ++;
		else if (!isdigit(*tokenptr & 255) && *tokenptr != '-')
		  break;

		values[num_values] = (int)strtol(tokenptr, &tokenptr, 0);
		num_values ++;
	      }

	      attrptr = ippAddIntegers(attrs, IPP_TAG_PRINTER, value, attr, num_values, values);
	    }

	    if (!tokenptr || *tokenptr)
	    {
	      serverLog(SERVER_LOGLEVEL_ERROR, "Bad %s value \"%s\" on line %d of \"%s\".", ippTagString(value), token, linenum, filename);
              goto load_error;
	    }
	    break;

	case IPP_TAG_RESOLUTION :
	    {
	      int	xres,		/* X resolution */
			yres;		/* Y resolution */
	      ipp_res_t	units;		/* Units */
	      char	*start,		/* Start of value */
			*ptr,		/* Pointer into value */
			*next = NULL;	/* Next value */

	      for (start = token; start; start = next)
	      {
		xres = yres = (int)strtol(start, (char **)&ptr, 10);
		if (ptr > start && xres > 0)
		{
		  if (*ptr == 'x')
		    yres = (int)strtol(ptr + 1, (char **)&ptr, 10);
		}

		if (ptr && (next = strchr(ptr, ',')) != NULL)
		  *next++ = '\0';

		if (ptr <= start || xres <= 0 || yres <= 0 || !ptr ||
		    (_cups_strcasecmp(ptr, "dpi") &&
		     _cups_strcasecmp(ptr, "dpc") &&
		     _cups_strcasecmp(ptr, "dpcm") &&
		     _cups_strcasecmp(ptr, "other")))
		{
		  serverLog(SERVER_LOGLEVEL_ERROR, "Bad resolution value \"%s\" on line %d of \"%s\".", token, linenum, filename);
                  goto load_error;
		}

		if (!_cups_strcasecmp(ptr, "dpc") || !_cups_strcasecmp(ptr, "dpcm"))
		  units = IPP_RES_PER_CM;
		else
		  units = IPP_RES_PER_INCH;

                if (attrptr)
		  ippSetResolution(attrs, &attrptr, ippGetCount(attrptr), units, xres, yres);
		else
		  attrptr = ippAddResolution(attrs, IPP_TAG_PRINTER, attr, units, xres, yres);
	      }
	    }
	    break;

	case IPP_TAG_RANGE :
	    {
	      int	lowers[4],	/* Lower value */
			uppers[4],	/* Upper values */
			num_vals;	/* Number of values */


	      num_vals = sscanf(token, "%d-%d,%d-%d,%d-%d,%d-%d",
				lowers + 0, uppers + 0,
				lowers + 1, uppers + 1,
				lowers + 2, uppers + 2,
				lowers + 3, uppers + 3);

	      if ((num_vals & 1) || num_vals == 0)
	      {
		serverLog(SERVER_LOGLEVEL_ERROR, "Bad rangeOfInteger value \"%s\" on line %d of \"%s\".", token, linenum, filename);
                goto load_error;
	      }

	      attrptr = ippAddRanges(attrs, IPP_TAG_PRINTER, attr, num_vals / 2, lowers,
				     uppers);
	    }
	    break;

	case IPP_TAG_BEGIN_COLLECTION :
	    if (!strcmp(token, "{"))
	    {
	      ipp_t	*col = get_collection(fp, filename, &linenum);
				    /* Collection value */

	      if (col)
	      {
		attrptr = ippAddCollection(attrs, IPP_TAG_PRINTER, attr, col);
		ippDelete(col);
	      }
	      else
		exit(1);
	    }
	    else
	    {
	      serverLog(SERVER_LOGLEVEL_ERROR, "Bad ATTR collection value on line %d of \"%s\".", linenum, filename);
              goto load_error;
	    }

	    do
	    {
	      ipp_t	*col;			/* Collection value */
	      long	pos = cupsFileTell(fp);	/* Save position of file */

	      if (!get_token(fp, token, sizeof(token), &linenum))
		break;

	      if (strcmp(token, ","))
	      {
		cupsFileSeek(fp, pos);
		break;
	      }

	      if (!get_token(fp, token, sizeof(token), &linenum) || strcmp(token, "{"))
	      {
		serverLog(SERVER_LOGLEVEL_ERROR, "Unexpected \"%s\" on line %d of \"%s\".", token, linenum, filename);
                goto load_error;
	      }

	      if ((col = get_collection(fp, filename, &linenum)) == NULL)
		break;

	      ippSetCollection(attrs, &attrptr, ippGetCount(attrptr), col);
	    }
	    while (!strcmp(token, "{"));
	    break;

	case IPP_TAG_STRING :
	    attrptr = ippAddOctetString(attrs, IPP_TAG_PRINTER, attr, token, (int)strlen(token));
	    break;

	default :
	    serverLog(SERVER_LOGLEVEL_ERROR, "Unsupported ATTR value tag %s on line %d of \"%s\".", ippTagString(value), linenum, filename);
            goto load_error;

	case IPP_TAG_TEXTLANG :
	case IPP_TAG_NAMELANG :
	case IPP_TAG_TEXT :
	case IPP_TAG_NAME :
	case IPP_TAG_KEYWORD :
	case IPP_TAG_URI :
	case IPP_TAG_URISCHEME :
	case IPP_TAG_CHARSET :
	case IPP_TAG_LANGUAGE :
	case IPP_TAG_MIMETYPE :
	    if (!strchr(token, ','))
	      attrptr = ippAddString(attrs, IPP_TAG_PRINTER, value, attr, NULL, token);
	    else
	    {
	     /*
	      * Multiple string values...
	      */

	      int	num_values;	/* Number of values */
	      char	*values[100],	/* Values */
			*ptr;		/* Pointer to next value */


	      values[0]  = token;
	      num_values = 1;

	      for (ptr = strchr(token, ','); ptr; ptr = strchr(ptr, ','))
	      {
		if (ptr > token && ptr[-1] == '\\')
		  _cups_strcpy(ptr - 1, ptr);
		else
		{
		  *ptr++ = '\0';
		  values[num_values] = ptr;
		  num_values ++;
		  if (num_values >= (int)(sizeof(values) / sizeof(values[0])))
		    break;
		}
	      }

	      attrptr = ippAddStrings(attrs, IPP_TAG_PRINTER, value, attr, num_values, NULL, (const char **)values);
	    }
	    break;
      }

      if (!attrptr)
      {
        serverLog(SERVER_LOGLEVEL_ERROR, "Unable to add attribute on line %d of \"%s\": %s", linenum, filename, cupsLastErrorString());
        goto load_error;
      }
    }
    else if (!_cups_strcasecmp(token, "AUTHTYPE") && authtype)
    {
      if (!get_token(fp, token, sizeof(token), &linenum))
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Missing AUTHTYPE value on line %d of \"%s\".", linenum, filename);
        goto load_error;
      }

      *authtype = strdup(token);
    }
    else if (!_cups_strcasecmp(token, "COMMAND") && command)
    {
      if (!get_token(fp, token, sizeof(token), &linenum))
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Missing COMMAND value on line %d of \"%s\".", linenum, filename);
        goto load_error;
      }

      *command = strdup(token);
    }
    else if (!_cups_strcasecmp(token, "DEVICE-URI") && device_uri)
    {
      if (!get_token(fp, token, sizeof(token), &linenum))
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Missing DEVICE-URI value on line %d of \"%s\".", linenum, filename);
        goto load_error;
      }

      *device_uri = strdup(token);
    }
    else if (!_cups_strcasecmp(token, "MAKE") && make)
    {
      if (!get_token(fp, token, sizeof(token), &linenum))
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Missing MAKE value on line %d of \"%s\".", linenum, filename);
        goto load_error;
      }

      *make = strdup(token);
    }
    else if (!_cups_strcasecmp(token, "MODEL") && model)
    {
      if (!get_token(fp, token, sizeof(token), &linenum))
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Missing MAKE value on line %d of \"%s\".", linenum, filename);
        goto load_error;
      }

      *model = strdup(token);
    }
    else if (!_cups_strcasecmp(token, "PROXY-USER") && proxy_user)
    {
      if (!get_token(fp, token, sizeof(token), &linenum))
      {
	serverLog(SERVER_LOGLEVEL_ERROR, "Missing PROXY-USER value on line %d of \"%s\".", linenum, filename);
        goto load_error;
      }

      *proxy_user = strdup(token);
    }
    else
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Unknown directive \"%s\" on line %d of \"%s\".", token, linenum, filename);
      goto load_error;
    }
  }

  cupsFileClose(fp);

  return (attrs);

 /*
  * If we get here something bad happened...
  */

  load_error:

  cupsFileClose(fp);

  ippDelete(attrs);

  return (NULL);
}


/*
 * 'serverLoadConfiguration()' - Load the server configuration file.
 */

int					/* O - 1 if successful, 0 on error */
serverLoadConfiguration(
    const char *directory)		/* I - Configuration directory */
{
  cups_dir_t	*dir;			/* Directory pointer */
  cups_dentry_t	*dent;			/* Directory entry */
  char		filename[260],		/* Configuration file/directory */
		resource[1024],		/* Resource path */
                *ptr;			/* Pointer into filename */
  server_printer_t *printer;		/* Printer */
  ipp_t		*attrs;			/* Printer attributes */
  char		*authtype,		/* AuthType value, if any */
		*command,		/* Command value, if any */
		*device_uri,		/* DeviceURI value, if any */
		*make,			/* Make value, if any */
		*model,			/* Model value, if any */
		*proxy_user;		/* ProxyUser value, if any */


 /*
  * First read the system configuration file, if any...
  */

  snprintf(filename, sizeof(filename), "%s/system.conf", directory);
  if (!load_system(filename))
    return (0);

 /*
  * Then see if there are any print queues...
  */

  snprintf(filename, sizeof(filename), "%s/print", directory);
  if ((dir = cupsDirOpen(filename)) != NULL)
  {
    while ((dent = cupsDirRead(dir)) != NULL)
    {
      if (fnmatch("*.conf", dent->filename, 0))
      {
       /*
        * Load the conf file, with any associated icon image.
        */

        strlcpy(filename, dent->filename, sizeof(filename));
        if ((ptr = filename + strlen(filename) - 5) > filename)
          strlcpy(ptr, ".png", sizeof(filename) - (size_t)(ptr - filename));

        if ((ptr = strrchr(dent->filename, '/')) != NULL)
	  ptr ++;
	else
	  continue;

        authtype = command = device_uri = make = model = proxy_user = NULL;

        if ((attrs = serverLoadAttributes(dent->filename, &authtype, &command, &device_uri, &make, &model, &proxy_user)) != NULL)
	{
          snprintf(resource, sizeof(resource), "/ipp/print/%s", ptr);

	  if ((printer = serverCreatePrinter(resource, ptr, NULL, make, model, access(filename, R_OK) ? NULL : filename, NULL, 0, 0, 0, 0, attrs, command, device_uri, proxy_user)) == NULL)
          continue;

	  if (!Printers)
	    Printers = cupsArrayNew((cups_array_func_t)compare_printers, NULL);

	  cupsArrayAdd(Printers, printer);
	}
      }
    }

    cupsDirClose(dir);
  }

  return (1);
}


/*
 * 'compare_printers()' - Compare two printers.
 */

static int				/* O - Result of comparison */
compare_printers(server_printer_t *a,	/* I - First printer */
                 server_printer_t *b)	/* I - Second printer */
{
  return (strcmp(a->resource, b->resource));
}


/*
 * 'get_collection()' - Get a collection value from a file.
 */

static ipp_t *				/* O  - Collection value */
get_collection(cups_file_t *fp,		/* I  - File to read from */
               const char  *filename,	/* I  - Attributes filename */
	       int         *linenum)	/* IO - Line number */
{
  char		token[1024],		/* Token from file */
		attr[128];		/* Attribute name */
  ipp_tag_t	value;			/* Current value type */
  ipp_t		*col = ippNew();	/* Collection value */
  ipp_attribute_t *lastcol = NULL;	/* Last collection attribute */


  while (get_token(fp, token, sizeof(token), linenum) != NULL)
  {
    if (!strcmp(token, "}"))
      break;
    else if (!strcmp(token, "{") && lastcol)
    {
     /*
      * Another collection value
      */

      ipp_t	*subcol = get_collection(fp, filename, linenum);
					/* Collection value */

      if (subcol)
        ippSetCollection(col, &lastcol, ippGetCount(lastcol), subcol);
      else
	goto col_error;
    }
    else if (!_cups_strcasecmp(token, "MEMBER"))
    {
     /*
      * Attribute...
      */

      lastcol = NULL;

      if (!get_token(fp, token, sizeof(token), linenum))
      {
	fprintf(stderr, "ippserver: Missing MEMBER value tag on line %d of \"%s\".\n", *linenum, filename);
	goto col_error;
      }

      if ((value = ippTagValue(token)) == IPP_TAG_ZERO)
      {
	fprintf(stderr, "ippserver: Bad MEMBER value tag \"%s\" on line %d of \"%s\".\n", token, *linenum, filename);
	goto col_error;
      }

      if (!get_token(fp, attr, sizeof(attr), linenum))
      {
	fprintf(stderr, "ippserver: Missing MEMBER name on line %d of \"%s\".\n", *linenum, filename);
	goto col_error;
      }

      if (!get_token(fp, token, sizeof(token), linenum))
      {
	fprintf(stderr, "ippserver: Missing MEMBER value on line %d of \"%s\".\n", *linenum, filename);
	goto col_error;
      }

      switch (value)
      {
	case IPP_TAG_BOOLEAN :
	    if (!_cups_strcasecmp(token, "true"))
	      ippAddBoolean(col, IPP_TAG_ZERO, attr, 1);
	    else
	      ippAddBoolean(col, IPP_TAG_ZERO, attr, (char)atoi(token));
	    break;

	case IPP_TAG_INTEGER :
	case IPP_TAG_ENUM :
	    ippAddInteger(col, IPP_TAG_ZERO, value, attr, atoi(token));
	    break;

	case IPP_TAG_RESOLUTION :
	    {
	      int	xres,		/* X resolution */
			yres;		/* Y resolution */
	      char	units[6];	/* Units */

	      if (sscanf(token, "%dx%d%5s", &xres, &yres, units) != 3 ||
		  (_cups_strcasecmp(units, "dpi") &&
		   _cups_strcasecmp(units, "dpc") &&
		   _cups_strcasecmp(units, "dpcm") &&
		   _cups_strcasecmp(units, "other")))
	      {
		fprintf(stderr, "ippserver: Bad resolution value \"%s\" on line %d of \"%s\".\n", token, *linenum, filename);
		goto col_error;
	      }

	      if (!_cups_strcasecmp(units, "dpi"))
		ippAddResolution(col, IPP_TAG_ZERO, attr, IPP_RES_PER_INCH, xres, yres);
	      else if (!_cups_strcasecmp(units, "dpc") ||
	               !_cups_strcasecmp(units, "dpcm"))
		ippAddResolution(col, IPP_TAG_ZERO, attr, IPP_RES_PER_CM, xres, yres);
	      else
		ippAddResolution(col, IPP_TAG_ZERO, attr, (ipp_res_t)0, xres, yres);
	    }
	    break;

	case IPP_TAG_RANGE :
	    {
	      int	lowers[4],	/* Lower value */
			uppers[4],	/* Upper values */
			num_vals;	/* Number of values */


	      num_vals = sscanf(token, "%d-%d,%d-%d,%d-%d,%d-%d",
				lowers + 0, uppers + 0,
				lowers + 1, uppers + 1,
				lowers + 2, uppers + 2,
				lowers + 3, uppers + 3);

	      if ((num_vals & 1) || num_vals == 0)
	      {
		fprintf(stderr, "ippserver: Bad rangeOfInteger value \"%s\" on line %d of \"%s\".\n", token, *linenum, filename);
		goto col_error;
	      }

	      ippAddRanges(col, IPP_TAG_ZERO, attr, num_vals / 2, lowers,
			   uppers);
	    }
	    break;

	case IPP_TAG_BEGIN_COLLECTION :
	    if (!strcmp(token, "{"))
	    {
	      ipp_t	*subcol = get_collection(fp, filename, linenum);
				      /* Collection value */

	      if (subcol)
	      {
		lastcol = ippAddCollection(col, IPP_TAG_ZERO, attr, subcol);
		ippDelete(subcol);
	      }
	      else
		goto col_error;
	    }
	    else
	    {
	      fprintf(stderr, "ippserver: Bad collection value on line %d of \"%s\".\n", *linenum, filename);
	      goto col_error;
	    }
	    break;
	case IPP_TAG_STRING :
	    ippAddOctetString(col, IPP_TAG_ZERO, attr, token, (int)strlen(token));
	    break;

	default :
	    if (!strchr(token, ','))
	      ippAddString(col, IPP_TAG_ZERO, value, attr, NULL, token);
	    else
	    {
	     /*
	      * Multiple string values...
	      */

	      int	num_values;	/* Number of values */
	      char	*values[100],	/* Values */
			*ptr;		/* Pointer to next value */


	      values[0]  = token;
	      num_values = 1;

	      for (ptr = strchr(token, ','); ptr; ptr = strchr(ptr, ','))
	      {
		*ptr++ = '\0';
		values[num_values] = ptr;
		num_values ++;
		if (num_values >= (int)(sizeof(values) / sizeof(values[0])))
		  break;
	      }

	      ippAddStrings(col, IPP_TAG_ZERO, value, attr, num_values,
			    NULL, (const char **)values);
	    }
	    break;
      }
    }
  }

  return (col);

 /*
  * If we get here there was a parse error; free memory and return.
  */

  col_error:

  ippDelete(col);

  return (NULL);
}


/*
 * 'get_token()' - Get a token from a file.
 */

static char *				/* O  - Token from file or NULL on EOF */
get_token(cups_file_t *fp,		/* I  - File to read from */
          char        *buf,		/* I  - Buffer to read into */
	  int         buflen,		/* I  - Length of buffer */
	  int         *linenum)		/* IO - Current line number */
{
  int	ch,				/* Character from file */
	quote;				/* Quoting character */
  char	*bufptr,			/* Pointer into buffer */
	*bufend;			/* End of buffer */


  for (;;)
  {
   /*
    * Skip whitespace...
    */

    while (isspace(ch = cupsFileGetChar(fp)))
    {
      if (ch == '\n')
        (*linenum) ++;
    }

   /*
    * Read a token...
    */

    if (ch == EOF)
      return (NULL);
    else if (ch == '\'' || ch == '\"')
    {
     /*
      * Quoted text or regular expression...
      */

      quote  = ch;
      bufptr = buf;
      bufend = buf + buflen - 1;

      while ((ch = cupsFileGetChar(fp)) != EOF)
      {
        if (ch == '\\')
	{
	 /*
	  * Escape next character...
	  */

	  if (bufptr < bufend)
	    *bufptr++ = (char)ch;

	  if ((ch = cupsFileGetChar(fp)) != EOF && bufptr < bufend)
	    *bufptr++ = (char)ch;
	}
	else if (ch == quote)
          break;
	else if (bufptr < bufend)
          *bufptr++ = (char)ch;
      }

      *bufptr = '\0';

      return (buf);
    }
    else if (ch == '#')
    {
     /*
      * Comment...
      */

      while ((ch = cupsFileGetChar(fp)) != EOF)
	if (ch == '\n')
          break;

      (*linenum) ++;
    }
    else if (ch == '{' || ch == '}' || ch == ',')
    {
      buf[0] = (char)ch;
      buf[1] = '\0';

      return (buf);
    }
    else
    {
     /*
      * Whitespace delimited text...
      */

      bufptr = buf;
      bufend = buf + buflen - 1;

      do
      {
	if (isspace(ch) || ch == '#')
          break;
	else if (bufptr < bufend)
          *bufptr++ = (char)ch;
      }
      while ((ch = cupsFileGetChar(fp)) != EOF);

      if (ch == '#')
      {
        while ((ch = cupsFileGetChar(fp)) != EOF)
        {
          if (ch == '\n')
          {
            (*linenum) ++;
            break;
          }
        }
      }
      else if (ch == '\n')
        (*linenum) ++;

      *bufptr = '\0';

      return (buf);
    }
  }
}


/*
 * 'load_system()' - Load the system configuration file.
 */

static int				/* O - 1 on success, 0 on failure */
load_system(const char *conf)		/* I - Configuration file */
{
  cups_file_t	*fp;			/* File pointer */
  int		status = 1,		/* Return value */
		linenum = 0;		/* Current line number */
  char		line[1024],		/* Line from file */
		*value;			/* Pointer to value on line */


  if ((fp = cupsFileOpen(conf, "r")) == NULL)
    return (errno == ENOENT);

  while (cupsFileGetConf(fp, line, sizeof(line), &value, &linenum))
  {
    if (!value)
    {
      fprintf(stderr, "ippserver: Missing value on line %d of \"%s\".\n", linenum, conf);
      status = 0;
      break;
    }

    if (_cups_strcasecmp(line, "DataDirectory"))
    {
      if (access(value, R_OK))
      {
        fprintf(stderr, "ippserver: Unable to access DataDirectory \"%s\": %s\n", value, strerror(errno));
        status = 0;
        break;
      }

      DataDirectory = strdup(value);
    }
    else if (_cups_strcasecmp(line, "Encryption"))
    {
      if (!_cups_strcasecmp(value, "always"))
        Encryption = HTTP_ENCRYPTION_ALWAYS;
      else if (!_cups_strcasecmp(value, "ifrequested"))
        Encryption = HTTP_ENCRYPTION_IF_REQUESTED;
      else if (!_cups_strcasecmp(value, "never"))
        Encryption = HTTP_ENCRYPTION_NEVER;
      else if (!_cups_strcasecmp(value, "required"))
        Encryption = HTTP_ENCRYPTION_REQUIRED;
      else
      {
        fprintf(stderr, "ippserver: Bad Encryption value \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }
    }
    else if (!_cups_strcasecmp(line, "KeepFiles"))
    {
      KeepFiles = !strcasecmp(value, "yes") || !strcasecmp(value, "true") || !strcasecmp(value, "on");
    }
    else if (!_cups_strcasecmp(line, "Listen"))
    {
      char	*ptr;			/* Pointer into host value */
      int	port;			/* Port number */

      if ((ptr = strrchr(value, ':')) != NULL && !isdigit(ptr[1] & 255))
      {
        fprintf(stderr, "ippserver: Bad Listen value \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }

      if (ptr)
      {
        *ptr++ = '\0';
        port   = atoi(ptr);
      }
      else
        port = 8000 + (getuid() % 1000);

      if (!serverCreateListeners(value, port))
      {
        status = 0;
        break;
      }
    }
    else if (!_cups_strcasecmp(line, "LogFile"))
    {
      if (!_cups_strcasecmp(value, "stderr"))
        LogFile = NULL;
      else
        LogFile = strdup(value);
    }
    else if (!strcasecmp(line, "LogLevel"))
    {
      if (!_cups_strcasecmp(value, "error"))
        LogLevel = SERVER_LOGLEVEL_ERROR;
      else if (!_cups_strcasecmp(value, "info"))
        LogLevel = SERVER_LOGLEVEL_INFO;
      else if (!_cups_strcasecmp(value, "debug"))
        LogLevel = SERVER_LOGLEVEL_DEBUG;
      else
      {
        fprintf(stderr, "ippserver: Bad LogLevel value \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }
    }
    else if (!_cups_strcasecmp(line, "MaxJobs"))
    {
      if (!isdigit(*value & 255))
      {
        fprintf(stderr, "ippserver: Bad MaxJobs value \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }

      MaxJobs = atoi(value);
    }
    else if (!_cups_strcasecmp(line, "SpoolDirectory"))
    {
      if (access(value, R_OK))
      {
        fprintf(stderr, "ippserver: Unable to access SpoolDirectory \"%s\": %s\n", value, strerror(errno));
        status = 0;
        break;
      }

      SpoolDirectory = strdup(value);
    }
    else
    {
      fprintf(stderr, "ippserver: Unknown directive \"%s\" on line %d.\n", line, linenum);
    }
  }

  cupsFileClose(fp);

  return (status);
}