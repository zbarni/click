// -*- c-basic-offset: 4 -*-
/*
 * click.cc -- user-level Click main program
 * Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
 * Copyright (c) 2001-2003 International Computer Science Institute
 * Copyright (c) 2004 Regents of the University of California
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/pathvars.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>

#include <click/lexer.hh>
#include <click/routerthread.hh>
#include <click/router.hh>
#include <click/master.hh>
#include <click/error.hh>
#include <click/timer.hh>
#include <click/straccum.hh>
#include <click/clp.h>
#include <click/archive.hh>
#include <click/glue.hh>
#include <click/driver.hh>
#include <click/userutils.hh>
#include <click/confparse.hh>
#include <click/handlercall.hh>
#include "elements/standard/quitwatcher.hh"
#include "elements/userlevel/controlsocket.hh"
CLICK_USING_DECLS

#define HELP_OPT		300
#define VERSION_OPT		301
#define CLICKPATH_OPT		302
#define ROUTER_OPT		303
#define EXPRESSION_OPT		304
#define QUIT_OPT		305
#define OUTPUT_OPT		306
#define HANDLER_OPT		307
#define TIME_OPT		308
#define PORT_OPT		310
#define UNIX_SOCKET_OPT		311
#define NO_WARNINGS_OPT		312
#define WARNINGS_OPT		313
#define ALLOW_RECONFIG_OPT	314
#define EXIT_HANDLER_OPT	315

static Clp_Option options[] = {
  { "allow-reconfigure", 'R', ALLOW_RECONFIG_OPT, 0, Clp_Negate },
  { "clickpath", 'C', CLICKPATH_OPT, Clp_ArgString, 0 },
  { "expression", 'e', EXPRESSION_OPT, Clp_ArgString, 0 },
  { "file", 'f', ROUTER_OPT, Clp_ArgString, 0 },
  { "handler", 'h', HANDLER_OPT, Clp_ArgString, 0 },
  { "help", 0, HELP_OPT, 0, 0 },
  { "output", 'o', OUTPUT_OPT, Clp_ArgString, 0 },
  { "port", 'p', PORT_OPT, Clp_ArgInt, 0 },
  { "quit", 'q', QUIT_OPT, 0, 0 },
  { "time", 't', TIME_OPT, 0, 0 },
  { "unix-socket", 'u', UNIX_SOCKET_OPT, Clp_ArgString, 0 },
  { "version", 'v', VERSION_OPT, 0, 0 },
  { "warnings", 0, WARNINGS_OPT, 0, Clp_Negate },
  { "exit-handler", 'x', EXIT_HANDLER_OPT, Clp_ArgString, 0 },
  { 0, 'w', NO_WARNINGS_OPT, 0, Clp_Negate },
};

static const char *program_name;

void
short_usage()
{
  fprintf(stderr, "Usage: %s [OPTION]... [ROUTERFILE]\n\
Try '%s --help' for more information.\n",
	  program_name, program_name);
}

void
usage()
{
  printf("\
'Click' runs a Click router configuration at user level. It installs the\n\
configuration, reporting any errors to standard error, and then generally runs\n\
until interrupted.\n\
\n\
Usage: %s [OPTION]... [ROUTERFILE]\n\
\n\
Options:\n\
  -f, --file FILE               Read router configuration from FILE.\n\
  -e, --expression EXPR         Use EXPR as router configuration.\n\
  -p, --port PORT               Listen for control connections on TCP port.\n\
  -u, --unix-socket FILE        Listen for control connections on Unix socket.\n\
  -R, --allow-reconfigure       Provide a writable 'hotconfig' handler.\n\
  -h, --handler ELEMENT.H       Call ELEMENT's read handler H after running\n\
                                driver and print result to standard output.\n\
  -x, --exit-handler ELEMENT.H  Use handler ELEMENT.H value for exit status.\n\
  -o, --output FILE             Write flat configuration to FILE.\n\
  -q, --quit                    Do not run driver.\n\
  -t, --time                    Print information on how long driver took.\n\
  -w, --no-warnings             Do not print warnings.\n\
  -C, --clickpath PATH          Use PATH for CLICKPATH.\n\
      --help                    Print this message and exit.\n\
  -v, --version                 Print version number and exit.\n\
\n\
Report bugs to <click@pdos.lcs.mit.edu>.\n", program_name);
}

static Router *router;
static ErrorHandler *errh;
static bool started = 0;

extern "C" {
static void
catch_signal(int sig)
{
  signal(sig, SIG_DFL);
  if (!started)
    kill(getpid(), sig);
  else
    router->adjust_runcount(-10000);
}
}


// report handler results

static int
call_read_handler(Element *e, String handler_name,
		  bool print_name, ErrorHandler *errh)
{
  const Handler *rh = Router::handler(e, handler_name);
  String full_name = Handler::unparse_name(e, handler_name);
  if (!rh || !rh->visible())
    return errh->error("no '%s' handler", full_name.cc());
  else if (!rh->read_visible())
    return errh->error("'%s' is a write handler", full_name.cc());

  if (print_name)
    fprintf(stdout, "%s:\n", full_name.cc());
  String result = rh->call_read(e);
  fputs(result.cc(), stdout);
  if (print_name)
    fputs("\n", stdout);

  return 0;
}

static int
expand_handler_elements(const String& pattern, const String& handler_name,
			Vector<Element*>& elements, Router* router)
{
    // first try element name
    if (Element* e = router->find(pattern)) {
	elements.push_back(e);
	return 1;
    }
    // check if we have a pattern
    bool is_pattern = false;
    for (const char* s = pattern.begin(); s < pattern.end(); s++)
	if (*s == '?' || *s == '*' || *s == '[') {
	    is_pattern = true;
	    break;
	}
    // check pattern or type
    bool any = false;
    for (int i = 0; i < router->nelements(); i++)
	if (is_pattern
	    ? glob_match(router->ename(i), pattern)
	    : router->element(i)->cast(pattern.c_str()) != 0) {
	    any = true;
	    const Handler* h = Router::handler(router->element(i), handler_name);
	    if (h && h->read_visible())
		elements.push_back(router->element(i));
	}
    if (!any)
	return errh->error((is_pattern ? "no element matching '%s'" : "no element '%s'"), pattern.c_str());
    else
	return 2;
}

static int
call_read_handlers(Vector<String> &handlers, ErrorHandler *errh)
{
    Vector<Element *> handler_elements;
    Vector<String> handler_names;
    bool print_names = (handlers.size() > 1);
    int before = errh->nerrors();

    // expand handler names
    for (int i = 0; i < handlers.size(); i++) {
	const char *dot = find(handlers[i], '.');
	if (dot == handlers[i].end()) {
	    call_read_handler(router->root_element(), handlers[i], print_names, errh);
	    continue;
	}
    
	String element_name = handlers[i].substring(handlers[i].begin(), dot);
	String handler_name = handlers[i].substring(dot + 1, handlers[i].end());

	Vector<Element*> elements;
	int retval = expand_handler_elements(element_name, handler_name, elements, router);
	if (retval >= 0)
	    for (int j = 0; j < elements.size(); j++)
		call_read_handler(elements[j], handler_name, print_names || retval > 1, errh);
    }

    return (errh->nerrors() == before ? 0 : -1);
}


// hotswapping

static Router *hotswap_router;
static Router *hotswap_thunk_router;
static bool hotswap_hook(Task *, void *);
static Task hotswap_task(hotswap_hook, 0);

static bool
hotswap_hook(Task *, void *)
{
    hotswap_router->activate(ErrorHandler::default_handler());
    // That step releases our reference to 'router'.
    router = hotswap_router;
    hotswap_router = 0;
    return true;
}

// switching configurations

static Vector<String> cs_unix_sockets;
static Vector<int> cs_ports;
static bool warnings = true;

static Router *
parse_configuration(const String &text, bool text_is_expr, bool hotswap,
		    ErrorHandler *errh)
{
  Router *r = click_read_router(text, text_is_expr, errh, false, (router ? router->master() : 0));
  if (!r)
    return 0;

  // add new ControlSockets
  String retries = (hotswap ? ", RETRIES 1, RETRY_WARNINGS false" : "");
  for (int i = 0; i < cs_ports.size(); i++)
    r->add_element(new ControlSocket, "click_driver@@ControlSocket@" + String(i), "tcp, " + String(cs_ports[i]) + retries, "click");
  for (int i = 0; i < cs_unix_sockets.size(); i++)
    r->add_element(new ControlSocket, "click_driver@@ControlSocket@" + String(i + cs_ports.size()), "unix, " + cp_quote(cs_unix_sockets[i]) + retries, "click");

  // catch signals (only need to do the first time)
  if (!hotswap) {
    // catch control-C and SIGTERM
    signal(SIGINT, catch_signal);
    signal(SIGTERM, catch_signal);
    // ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
  }

  // register hotswap router on new router
  if (hotswap && router && router->initialized())
    r->set_hotswap_router(router);
  
  if (errh->nerrors() > 0 || r->initialize(errh) < 0) {
    if (hotswap && router && router->initialized())
      router->use();		// Account for 'r' reference to 'router'
    delete r;
    return 0;
  } else
    return r;
}

static int
hotconfig_handler(const String &text, Element *, void *, ErrorHandler *errh)
{
  if (Router *q = parse_configuration(text, true, true, errh)) {
    if (hotswap_router)
      hotswap_router->unuse();
    hotswap_router = q;
    hotswap_task.reschedule();
    return 0;
  } else
    return -EINVAL;
}


// main

int
main(int argc, char **argv)
{
  click_static_initialize();
  errh = ErrorHandler::default_handler();

  // read command line arguments
  Clp_Parser *clp =
    Clp_NewParser(argc, argv, sizeof(options) / sizeof(options[0]), options);
  program_name = Clp_ProgramName(clp);

  const char *router_file = 0;
  bool file_is_expr = false;
  const char *output_file = 0;
  bool quit_immediately = false;
  bool report_time = false;
  bool allow_reconfigure = false;
  Vector<String> handlers;
  String exit_handler;

  while (1) {
    int opt = Clp_Next(clp);
    switch (opt) {
      
     case ROUTER_OPT:
     case EXPRESSION_OPT:
     case Clp_NotOption:
      if (router_file) {
	errh->error("router configuration specified twice");
	goto bad_option;
      }
      router_file = clp->arg;
      file_is_expr = (opt == EXPRESSION_OPT);
      break;
      
     case OUTPUT_OPT:
      if (output_file) {
	errh->error("output file specified twice");
	goto bad_option;
      }
      output_file = clp->arg;
      break;
     
     case HANDLER_OPT:
      handlers.push_back(clp->arg);
      break;

     case EXIT_HANDLER_OPT:
      if (exit_handler) {
	errh->error("--exit-handler specified twice");
	goto bad_option;
      }
      exit_handler = clp->arg;
      break;
      
     case PORT_OPT:
      cs_ports.push_back(clp->val.i);
      break;

     case UNIX_SOCKET_OPT:
      cs_unix_sockets.push_back(clp->arg);
      break;

     case ALLOW_RECONFIG_OPT:
      allow_reconfigure = !clp->negated;
      break;
      
     case QUIT_OPT:
      quit_immediately = true;
      break;

     case TIME_OPT:
      report_time = true;
      break;

     case WARNINGS_OPT:
      warnings = !clp->negated;
      break;

     case NO_WARNINGS_OPT:
      warnings = clp->negated;
      break;

     case CLICKPATH_OPT:
      set_clickpath(clp->arg);
      break;
      
     case HELP_OPT:
      usage();
      exit(0);
      break;
      
     case VERSION_OPT:
      printf("click (Click) %s\n", CLICK_VERSION);
      printf("Copyright (C) 1999-2001 Massachusetts Institute of Technology\n\
Copyright (C) 2001-2003 International Computer Science Institute\n\
Copyright (C) 2004 Regents of the University of California\n\
This is free software; see the source for copying conditions.\n\
There is NO warranty, not even for merchantability or fitness for a\n\
particular purpose.\n");
      exit(0);
      break;
      
     bad_option:
     case Clp_BadOption:
      short_usage();
      exit(1);
      break;
      
     case Clp_Done:
      goto done;
      
    }
  }
  
 done:
  // provide hotconfig handler if asked
  if (allow_reconfigure)
    Router::add_write_handler(0, "hotconfig", hotconfig_handler, 0);

  // parse configuration
  router = parse_configuration(router_file, file_is_expr, false, errh);
  if (!router)
    exit(1);

  int exit_value = 0;

  // output flat configuration
  if (output_file) {
    FILE *f = 0;
    if (strcmp(output_file, "-") != 0) {
      f = fopen(output_file, "w");
      if (!f) {
	errh->error("%s: %s", output_file, strerror(errno));
	exit_value = 1;
      }
    } else
      f = stdout;
    if (f) {
      Element *root = router->root_element();
      String s = Router::handler(root, "flatconfig")->call_read(root);
      fwrite(s.data(), 1, s.length(), f);
      if (f != stdout)
	fclose(f);
    }
  }

  struct rusage before, after;
  struct timeval before_time, after_time;
  getrusage(RUSAGE_SELF, &before);
  gettimeofday(&before_time, 0);

  // run driver
  // 10.Apr.2004 - Don't run the router if it has no elements.
  if (!quit_immediately && router->nelements()) {
    started = true;
    router->activate(errh);
    if (allow_reconfigure) {
      hotswap_thunk_router = new Router("", router->master());
      hotswap_thunk_router->initialize(errh);
      hotswap_task.initialize(hotswap_thunk_router, false);
      hotswap_thunk_router->activate(false, errh);
    }
    router->master()->thread(0)->driver();
  } else if (!quit_immediately && warnings)
    errh->warning("%s: configuration has no elements, exiting", filename_landmark(router_file, file_is_expr));

  gettimeofday(&after_time, 0);
  getrusage(RUSAGE_SELF, &after);
  // report time
  if (report_time) {
    struct timeval diff;
    timersub(&after.ru_utime, &before.ru_utime, &diff);
    printf("%ld.%03ldu", (long)diff.tv_sec, (long)((diff.tv_usec+500)/1000));
    timersub(&after.ru_stime, &before.ru_stime, &diff);
    printf(" %ld.%03lds", (long)diff.tv_sec, (long)((diff.tv_usec+500)/1000));
    timersub(&after_time, &before_time, &diff);
    printf(" %ld:%02ld.%02ld", (long)(diff.tv_sec/60), (long)(diff.tv_sec%60), (long)((diff.tv_usec+5000)/10000));
    printf("\n");
  }

  // call handlers
  if (handlers.size())
    if (call_read_handlers(handlers, errh) < 0)
      exit_value = 1;

  // call exit handler
  if (exit_handler) {
    int before = errh->nerrors();
    String exit_string = HandlerCall::call_read(exit_handler, router, errh);
    bool b;
    if (errh->nerrors() != before)
      exit_value = -1;
    else if (cp_integer(cp_uncomment(exit_string), &exit_value))
      /* nada */;
    else if (cp_bool(cp_uncomment(exit_string), &b))
      exit_value = (b ? 0 : 1);
    else {
      errh->error("exit handler value should be integer");
      exit_value = -1;
    }
  }

  delete router->master();
  click_static_cleanup();
  Clp_DeleteParser(clp);
  exit(exit_value);
  // Will leave String objects allocated because of 'handlers' and
  // 'exit_handler' above
}
