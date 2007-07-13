/*
 * "$Id$"
 *
 *   CUPS filtering program for the Common UNIX Printing System (CUPS).
 *
 *   Copyright 2007 by Apple Inc.
 *   Copyright 1997-2006 by Easy Software Products, all rights reserved.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Apple Inc. and are protected by Federal copyright
 *   law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 *   which should have been included with this file.  If this file is
 *   file is missing or damaged, see the license at "http://www.cups.org/".
 *
 * Contents:
 *
 *   main()            - Main entry for the test program.
 *   compare_pids()    - Compare two filter PIDs...
 *   escape_options()  - Convert an options array to a string.
 *   exec_filter()     - Execute a single filter.
 *   exec_filters()    - Execute filters for the given file and options.
 *   open_pipe()       - Create a pipe which is closed on exec.
 *   read_cupsd_conf() - Read the cupsd.conf file to get the filter settings.
 *   set_string()      - Copy and set a string.
 *   usage()           - Show program usage...
 */

/*
 * Include necessary headers...
 */

#include <cups/cups.h>
#include <cups/i18n.h>
#include <cups/string.h>
#include <errno.h>
#include "mime.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#  include <libgen.h>
#endif /* __APPLE__ */ 


/*
 * Local globals...
 */

static char		*DataDir = NULL;/* CUPS_DATADIR environment variable */
static char		*FontPath = NULL;
					/* CUPS_FONTPATH environment variable */
static mime_filter_t	GZIPFilter =	/* gziptoany filter */
{
  NULL,					/* Source type */
  NULL,					/* Destination type */
  0,					/* Cost */
  "gziptoany"				/* Filter program to run */
};
static char		*Path = NULL;	/* PATH environment variable */
static char		*ServerBin = NULL;
					/* CUPS_SERVERBIN environment variable */
static char		*ServerRoot = NULL;
					/* CUPS_SERVERROOT environment variable */
static char		*RIPCache = NULL;
					/* RIP_CACHE environment variable */


/*
 * Local functions...
 */

static int	compare_pids(mime_filter_t *a, mime_filter_t *b);
static char	*escape_options(int num_options, cups_option_t *options);
static int	exec_filter(const char *filter, char **argv, char **envp,
		            int infd, int outfd);
static int	exec_filters(cups_array_t *filters, const char *filename,
		             const char *ppdfile, const char *title,
			     int num_options, cups_option_t *options);
static int	open_pipe(int *fds);
static int	read_cupsd_conf(const char *filename);
static void	set_string(char **s, const char *val);
static void	usage(const char *opt);


/*
 * 'main()' - Main entry for the test program.
 */

int					/* O - Exit status */
main(int  argc,				/* I - Number of command-line args */
     char *argv[])			/* I - Command-line arguments */
{
  int		i;			/* Looping vars */
  const char	*opt;			/* Current option */
  char		super[MIME_MAX_SUPER],	/* Super-type name */
		type[MIME_MAX_TYPE];	/* Type name */
  int		compression;		/* Compression of file */
  int		cost;			/* Cost of filters */
  mime_t	*mime;			/* MIME database */
  char		*filename;		/* File to filter */
  char		cupsdconf[1024];	/* cupsd.conf file */
  const char	*server_root;		/* CUPS_SERVERROOT environment variable */
  mime_type_t	*src,			/* Source type */
		*dst;			/* Destination type */
  cups_array_t	*filters;		/* Filters for the file */
  int		num_options;		/* Number of options */
  cups_option_t	*options;		/* Options */
  const char	*ppdfile;		/* PPD file */
  const char	*title;			/* Title string */


 /*
  * Setup defaults...
  */

  mime        = NULL;
  src         = NULL;
  dst         = NULL;
  filename    = NULL;
  num_options = 0;
  options     = NULL;
  ppdfile     = NULL;
  title       = NULL;
  super[0]    = '\0';
  type[0]     = '\0';

  if ((server_root = getenv("CUPS_SERVERROOT")) == NULL)
    server_root = CUPS_SERVERROOT;

  snprintf(cupsdconf, sizeof(cupsdconf), "%s/cupsd.conf", server_root);

 /*
  * Process command-line arguments...
  */

  _cupsSetLocale(argv);

  for (i = 1; i < argc; i ++)
    if (argv[i][0] == '-')
    {
      for (opt = argv[i] + 1; *opt; opt ++)
        switch (*opt)
	{
	  case '-' : /* Next argument is a filename... */
	      i ++;
	      if (i < argc && !filename)
	        filename = argv[i];
	      else
	        usage(opt);
	      break;

          case 'c' : /* Specify cupsd.conf file location... */
	      i ++;
	      if (i < argc)
	        strlcpy(cupsdconf, argv[i], sizeof(cupsdconf));
	      else
	        usage(opt);
	      break;

          case 'm' : /* Specify destination MIME type... */
	      i ++;
	      if (i < argc)
	      {
	        if (sscanf(argv[i], "%15[^/]/%255s", super, type) != 2)
		  usage(opt);
	      }
	      else
	        usage(opt);
	      break;

          case 'n' : /* Specify number of copies... */
	      i ++;
	      if (i < argc)
	        num_options = cupsAddOption("copies", argv[i], num_options,
		                            &options);
	      else
	        usage(opt);
	      break;

          case 'o' : /* Specify option... */
	      i ++;
	      if (i < argc)
	        num_options = cupsParseOptions(argv[i], num_options, &options);
	      else
	        usage(opt);
	      break;

          case 'p' : /* Specify PPD file... */
	      i ++;
	      if (i < argc)
	        ppdfile = argv[i];
	      else
	        usage(opt);
	      break;

          case 't' : /* Specify number of copies... */
	      i ++;
	      if (i < argc)
	        title = argv[i];
	      else
	        usage(opt);
	      break;

	  default : /* Something we don't understand... */
	      usage(opt);
	      break;
	}
    }
    else if (!filename)
      filename = argv[i];
    else
    {
      _cupsLangPuts(stderr,
                    _("cupsfilter: Only one filename can be specified!\n"));
      usage(NULL);
    }

  if (!filename || !super[0] || !type[0])
    usage(NULL);

  if (!title)
  {
    if ((title = strrchr(filename, '/')) != NULL)
      title ++;
    else
      title = filename;
  }

 /*
  * Load the cupsd.conf file and create the MIME database...
  */

  if (read_cupsd_conf(cupsdconf))
    return (1);

  if ((mime = mimeLoad(ServerRoot, Path)) == NULL)
  {
    _cupsLangPrintf(stderr,
                    _("cupsfilter: Unable to read MIME database from \"%s\"!\n"),
		    ServerRoot);
    return (1);
  }

 /*
  * Get the source and destination types...
  */

  if ((src = mimeFileType(mime, filename, filename, &compression)) == NULL)
  {
    _cupsLangPrintf(stderr,
                    _("cupsfilter: Unable to determine MIME type of \"%s\"!\n"),
		    filename);
    return (1);
  }

  if ((dst = mimeType(mime, super, type)) == NULL)
  {
    _cupsLangPrintf(stderr,
                    _("cupsfilter: Unknown destination MIME type %s/%s!\n"),
		    super, type);
    return (1);
  }

 /*
  * Figure out how to filter the file...
  */

  if (src == dst)
  {
   /*
    * Special case - no filtering needed...
    */

    filters = cupsArrayNew(NULL, NULL);
    cupsArrayAdd(filters, &GZIPFilter);
  }
  else if ((filters = mimeFilter(mime, src, dst, &cost)) == NULL)
  {
    _cupsLangPrintf(stderr,
                    _("cupsfilter: No filter to convert from %s/%s to %s/%s!\n"),
		    src->super, src->type, dst->super, dst->type);
    return (1);
  }
  else if (compression)
    cupsArrayInsert(filters, &GZIPFilter);

 /*
  * Do it!
  */

  return (exec_filters(filters, filename, ppdfile, title, num_options, options));
}


/*
 * 'compare_pids()' - Compare two filter PIDs...
 */

static int				/* O - Result of comparison */
compare_pids(mime_filter_t *a,		/* I - First filter */
             mime_filter_t *b)		/* I - Second filter */
{
 /*
  * Because we're particularly lazy, we store the process ID in the "cost"
  * variable...
  */

  return (a->cost - b->cost);
}


/*
 * 'escape_options()' - Convert an options array to a string.
 */

static char *				/* O - Option string */
escape_options(
    int           num_options,		/* I - Number of options */
    cups_option_t *options)		/* I - Options */
{
  int		i;			/* Looping var */
  cups_option_t	*option;		/* Current option */
  int		bytes;			/* Number of bytes needed */
  char		*s,			/* Option string */
		*sptr,			/* Pointer into string */
		*vptr;			/* Pointer into value */


 /*
  * Figure out the worst-case number of bytes we need for the option string.
  */

  for (i = num_options, option = options, bytes = 1; i > 0; i --, option ++)
    bytes += 2 * (strlen(option->name) + strlen(option->value)) + 2;

  s = malloc(bytes);

 /*
  * Copy the options to the string...
  */

  for (i = num_options, option = options, sptr = s; i > 0; i --, option ++)
  {
    if (!strcmp(option->name, "copies"))
      continue;

    if (sptr > s)
      *sptr++ = ' ';

    strcpy(sptr, option->name);
    sptr += strlen(sptr);
    *sptr++ = '=';

    for (vptr = option->value; *vptr;)
    {
      if (strchr("\\ \t\n", *vptr))
        *sptr++ = '\\';

      *sptr++ = *vptr++;
    }
  }

  *sptr = '\0';

  fprintf(stderr, "DEBUG: options=\"%s\"\n", s);

  return (s);
}


/*
 * 'exec_filter()' - Execute a single filter.
 */

static int				/* O - Process ID or -1 on error */
exec_filter(const char *filter,		/* I - Filter to execute */
            char       **argv,		/* I - Argument list */
	    char       **envp,		/* I - Environment list */
	    int        infd,		/* I - Stdin file descriptor */
	    int        outfd)		/* I - Stdout file descriptor */
{
  int		pid;			/* Process ID */
#if defined(__APPLE__)
  char		processPath[1024],	/* CFProcessPath environment variable */
		linkpath[1024];		/* Link path for symlinks... */
  int		linkbytes;		/* Bytes for link path */


 /*
  * Add special voodoo magic for MacOS X - this allows MacOS X 
  * programs to access their bundle resources properly...
  */

  if ((linkbytes = readlink(filter, linkpath, sizeof(linkpath) - 1)) > 0)
  {
   /*
    * Yes, this is a symlink to the actual program, nul-terminate and
    * use it...
    */

    linkpath[linkbytes] = '\0';

    if (linkpath[0] == '/')
      snprintf(processPath, sizeof(processPath), "CFProcessPath=%s",
	       linkpath);
    else
      snprintf(processPath, sizeof(processPath), "CFProcessPath=%s/%s",
	       dirname((char *)filter), linkpath);
  }
  else
    snprintf(processPath, sizeof(processPath), "CFProcessPath=%s", filter);

  envp[0] = processPath;		/* Replace <CFProcessPath> string */
#endif	/* __APPLE__ */

  if ((pid = fork()) == 0)
  {
   /*
    * Child process goes here...
    *
    * Update stdin/stdout/stderr as needed...
    */

    if (infd != 0)
    {
      close(0);
      if (infd > 0)
        dup(infd);
      else
        open("/dev/null", O_RDONLY);
    }

    if (outfd != 1)
    {
      close(1);
      if (outfd > 0)
	dup(outfd);
      else
        open("/dev/null", O_WRONLY);
    }

    close(3);
    open("/dev/null", O_RDWR);
    fcntl(3, F_SETFL, O_NDELAY);

    close(4);
    open("/dev/null", O_RDWR);
    fcntl(4, F_SETFL, O_NDELAY);

   /*
    * Execute command...
    */

    execve(filter, argv, envp);

    perror(filter);

    exit(errno);
  }

  return (pid);
}


/*
 * 'exec_filters()' - Execute filters for the given file and options.
 */

static int				/* O - 0 on success, 1 on error */
exec_filters(cups_array_t  *filters,	/* I - Array of filters to run */
             const char    *filename,	/* I - File to filter */
	     const char    *ppdfile,	/* I - PPD file, if any */
	     const char    *title,	/* I - Job title */
             int           num_options,	/* I - Number of filter options */
	     cups_option_t *options)	/* I - Filter options */
{
  const char	*argv[8],		/* Command-line arguments */
		*envp[11],		/* Environment variables */
		*temp;			/* Temporary string */
  char		*optstr,		/* Filter options */
		cups_datadir[1024],	/* CUPS_DATADIR */
		cups_fontpath[1024],	/* CUPS_FONTPATH */
		cups_serverbin[1024],	/* CUPS_SERVERBIN */
		cups_serverroot[1024],	/* CUPS_SERVERROOT */
		lang[1024],		/* LANG */
		path[1024],		/* PATH */
		ppd[1024],		/* PPD */
		rip_cache[1024],	/* RIP_CACHE */
		user[1024],		/* USER */
		program[1024];		/* Program to run */
  mime_filter_t	*filter,		/* Current filter */
		*next;			/* Next filter */
  int		current,		/* Current filter */
		filterfds[2][2],	/* Pipes for filters */
		pid,			/* Process ID of filter */
		status,			/* Exit status */
		retval;			/* Return value */
  cups_array_t	*pids;			/* Executed filters array */
  mime_filter_t	key;			/* Search key for filters */


 /*
  * Setup the filter environment and command-line...
  */

  optstr = escape_options(num_options, options);

  snprintf(cups_datadir, sizeof(cups_datadir), "CUPS_DATADIR=%s", DataDir);
  snprintf(cups_fontpath, sizeof(cups_fontpath), "CUPS_FONTPATH=%s", FontPath);
  snprintf(cups_serverbin, sizeof(cups_serverbin), "CUPS_SERVERBIN=%s",
           ServerBin);
  snprintf(cups_serverroot, sizeof(cups_serverroot), "CUPS_SERVERROOT=%s",
           ServerRoot);
  if ((temp = getenv("LANG")) != NULL)
    snprintf(lang, sizeof(lang), "LANG=%s", temp);
  else if ((temp = getenv("LC_ALL")) != NULL)
    snprintf(lang, sizeof(lang), "LC_ALL=%s", temp);
  else
    strcpy(lang, "LANG=C");
  snprintf(path, sizeof(path), "PATH=%s", Path);
  if (ppdfile)
    snprintf(ppd, sizeof(ppd), "PPD=%s", ppdfile);
  else if ((temp = getenv("PPD")) != NULL)
    snprintf(ppd, sizeof(ppd), "PPD=%s", temp);
  else
    snprintf(ppd, sizeof(ppd), "PPD=%s/model/laserjet.ppd", DataDir);
  snprintf(rip_cache, sizeof(rip_cache), "RIP_CACHE=%s", RIPCache);
  snprintf(user, sizeof(user), "USER=%s", cupsUser());

  argv[0] = "cupsfilter";
  argv[1] = "0";
  argv[2] = cupsUser();
  argv[3] = title;
  argv[4] = cupsGetOption("copies", num_options, options);
  argv[5] = optstr;
  argv[6] = filename;
  argv[7] = NULL;

  if (!argv[4])
    argv[4] = "1";

  envp[0]  = "<CFProcessPath>";
  envp[1]  = cups_datadir;
  envp[2]  = cups_fontpath;
  envp[3]  = cups_serverbin;
  envp[4]  = cups_serverroot;
  envp[5]  = lang;
  envp[6]  = path;
  envp[7]  = ppd;
  envp[8]  = rip_cache;
  envp[9]  = user;
  envp[10] = NULL;

 /*
  * Execute all of the filters...
  */

  pids            = cupsArrayNew((cups_array_func_t)compare_pids, NULL);
  current         = 0;
  filterfds[0][0] = -1;
  filterfds[0][1] = -1;
  filterfds[1][0] = -1;
  filterfds[1][1] = -1;

  for (filter = (mime_filter_t *)cupsArrayFirst(filters);
       filter;
       filter = next, current = 1 - current)
  {
    next = (mime_filter_t *)cupsArrayNext(filters);

    if (filter->filter[0] == '/')
      strlcpy(program, filter->filter, sizeof(program));
    else
      snprintf(program, sizeof(program), "%s/filter/%s", ServerBin,
	       filter->filter);

    if (filterfds[!current][1] > 1)
    {
      close(filterfds[1 - current][0]);
      close(filterfds[1 - current][1]);

      filterfds[1 - current][0] = -1;
      filterfds[1 - current][0] = -1;
    }

    if (next)
      open_pipe(filterfds[1 - current]);
    else
      filterfds[1 - current][1] = 1;

    pid = exec_filter(program, (char **)argv, (char **)envp,
                      filterfds[current][0], filterfds[1 - current][1]);

    if (pid > 0)
    {
      fprintf(stderr, "INFO: %s (PID %d) started.\n", filter->filter, pid);

      filter->cost = pid;
      cupsArrayAdd(pids, filter);
    }
    else
      break;

    argv[6] = NULL;
  }

 /*
  * Close remaining pipes...
  */

  if (filterfds[0][1] > 1)
  {
    close(filterfds[0][0]);
    close(filterfds[0][1]);
  }

  if (filterfds[1][1] > 1)
  {
    close(filterfds[1][0]);
    close(filterfds[1][1]);
  }

 /*
  * Wait for the children to exit...
  */

  retval = 0;

  while (cupsArrayCount(pids) > 0)
  {
    if ((pid = wait(&status)) < 0)
      continue;

    key.cost = pid;
    if ((filter = (mime_filter_t *)cupsArrayFind(pids, &key)) != NULL)
    {
      cupsArrayRemove(pids, filter);

      if (status)
      {
	if (WIFEXITED(status))
	  fprintf(stderr, "ERROR: %s (PID %d) stopped with status %d!\n",
		  filter->filter, pid, WEXITSTATUS(status));
	else
	  fprintf(stderr, "ERROR: %s (PID %d) crashed on signal %d!\n",
		  filter->filter, pid, WTERMSIG(status));

        retval = 1;
      }
      else
        fprintf(stderr, "INFO: %s (PID %d) exited with no errors.\n",
	        filter->filter, pid);
    }
  }

  return (retval);
}


/*
 * 'open_pipe()' - Create a pipe which is closed on exec.
 */

int					/* O - 0 on success, -1 on error */
open_pipe(int *fds)			/* O - Pipe file descriptors (2) */
{
 /*
  * Create the pipe...
  */

  if (pipe(fds))
  {
    fds[0] = -1;
    fds[1] = -1;

    return (-1);
  }

 /*
  * Set the "close on exec" flag on each end of the pipe...
  */

  if (fcntl(fds[0], F_SETFD, fcntl(fds[0], F_GETFD) | FD_CLOEXEC))
  {
    close(fds[0]);
    close(fds[1]);

    fds[0] = -1;
    fds[1] = -1;

    return (-1);
  }

  if (fcntl(fds[1], F_SETFD, fcntl(fds[1], F_GETFD) | FD_CLOEXEC))
  {
    close(fds[0]);
    close(fds[1]);

    fds[0] = -1;
    fds[1] = -1;

    return (-1);
  }

 /*
  * Return 0 indicating success...
  */

  return (0);
}


/*
 * 'read_cupsd_conf()' - Read the cupsd.conf file to get the filter settings.
 */

static int				/* O - 0 on success, 1 on error */
read_cupsd_conf(const char *filename)	/* I - File to read */
{
  const char	*temp;			/* Temporary string */
  char		line[1024],		/* Line from file */
		*ptr;			/* Pointer into line */


  if ((temp = getenv("CUPS_DATADIR")) != NULL)
    set_string(&DataDir, temp);
  else
    set_string(&DataDir, CUPS_DATADIR);

  if ((temp = getenv("CUPS_FONTPATH")) != NULL)
    set_string(&FontPath, temp);
  else
    set_string(&FontPath, CUPS_FONTPATH);

  if ((temp = getenv("CUPS_SERVERBIN")) != NULL)
    set_string(&ServerBin, temp);
  else
    set_string(&ServerBin, CUPS_SERVERBIN);

  strlcpy(line, filename, sizeof(line));
  if ((ptr = strrchr(line, '/')) != NULL)
    *ptr = '\0';
  else
    getcwd(line, sizeof(line));

  set_string(&ServerRoot, line);

  snprintf(line, sizeof(line),
           "%s/filter:" CUPS_BINDIR ":" CUPS_SBINDIR ":/bin/usr/bin",
	   ServerBin);
  set_string(&Path, line);

  return (0);
}


/*
 * 'set_string()' - Copy and set a string.
 */

static void
set_string(char       **s,		/* O - Copy of string */
           const char *val)		/* I - String to copy */
{
  if (*s)
    free(*s);

  *s = strdup(val);
}


/*
 * 'usage()' - Show program usage...
 */

static void
usage(const char *opt)			/* I - Incorrect option, if any */
{
  if (opt)
    _cupsLangPrintf(stderr, _("%s: Unknown option '%c'!\n"), "cupsfilter",
                    *opt);

  _cupsLangPuts(stdout,
                _("Usage: cupsfilter -m mime/type [ options ] filename(s)\n"
		  "\n"
		  "Options:\n"
		  "\n"
		  "  -c cupsd.conf    Set cupsd.conf file to use\n"
		  "  -n copies        Set number of copies\n"
		  "  -o name=value    Set option(s)\n"
		  "  -p filename.ppd  Set PPD file\n"
		  "  -t title         Set title\n"));

  exit(1);
}


/*
 * End of "$Id$".
 */