/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright (C) 2004-2008 Constantin Kaplinsky.  All Rights Reserved.
 *    
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

// FIXME: Check cases when screen width/height is not a multiply of 32.
//        e.g. 800x600.

#include <strings.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <rfb/Logger_stdio.h>
#include <rfb/LogWriter.h>
#include <rfb/VNCServerSpawnX.h>
#include <rfb/Configuration.h>
#include <rfb/Timer.h>
#include <network/TcpSocket.h>
#include <network/UnixSocket.h>

#include <signal.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <spawnvncserver/XDesktop.h>
#include <spawnvncserver/Geometry.h>

extern char buildtime[];

using namespace rfb;
using namespace network;

static LogWriter vlog("Main");

IntParameter pollingCycle("PollingCycle", "Milliseconds per one polling "
                          "cycle; actual interval may be dynamically "
                          "adjusted to satisfy MaxProcessorUsage setting", 30);
IntParameter maxProcessorUsage("MaxProcessorUsage", "Maximum percentage of "
                               "CPU time to be consumed", 35);
StringParameter displayname("display", "The X display", "");
IntParameter rfbport("rfbport", "TCP port to listen for RFB protocol",5900);
StringParameter rfbunixpath("rfbunixpath", "Unix socket to listen for RFB protocol", "");
IntParameter rfbunixmode("rfbunixmode", "Unix socket access mode", 0600);
StringParameter hostsFile("HostsFile", "File with IP access control rules", "");
BoolParameter localhostOnly("localhost",
                            "Only allow connections from localhost",
                            false);

//
// Allow the main loop terminate itself gracefully on receiving a signal.
//

static bool caughtSignal = false;

static void CleanupSignalHandler(int sig)
{
  caughtSignal = true;
}


class FileTcpFilter : public TcpFilter
{

public:

  FileTcpFilter(const char *fname)
    : TcpFilter("-"), fileName(NULL), lastModTime(0)
  {
    if (fname != NULL)
      fileName = strdup((char *)fname);
  }

  virtual ~FileTcpFilter()
  {
    if (fileName != NULL)
      free(fileName);
  }

  virtual bool verifyConnection(Socket* s)
  {
    if (!reloadRules()) {
      vlog.error("Could not read IP filtering rules: rejecting all clients");
      filter.clear();
      filter.push_back(parsePattern("-"));
      return false;
    }

    return TcpFilter::verifyConnection(s);
  }

protected:

  bool reloadRules()
  {
    if (fileName == NULL)
      return true;

    struct stat st;
    if (stat(fileName, &st) != 0)
      return false;

    if (st.st_mtime != lastModTime) {
      // Actually reload only if the file was modified
      FILE *fp = fopen(fileName, "r");
      if (fp == NULL)
        return false;

      // Remove all the rules from the parent class
      filter.clear();

      // Parse the file contents adding rules to the parent class
      char buf[32];
      while (readLine(buf, 32, fp)) {
        if (buf[0] && strchr("+-?", buf[0])) {
          filter.push_back(parsePattern(buf));
        }
      }

      fclose(fp);
      lastModTime = st.st_mtime;
    }
    return true;
  }

protected:

  char *fileName;
  time_t lastModTime;

private:

  //
  // NOTE: we silently truncate long lines in this function.
  //

  bool readLine(char *buf, int bufSize, FILE *fp)
  {
    if (fp == NULL || buf == NULL || bufSize == 0)
      return false;

    if (fgets(buf, bufSize, fp) == NULL)
      return false;

    char *ptr = strchr(buf, '\n');
    if (ptr != NULL) {
      *ptr = '\0';              // remove newline at the end
    } else {
      if (!feof(fp)) {
        int c;
        do {                    // skip the rest of a long line
          c = getc(fp);
        } while (c != '\n' && c != EOF);
      }
    }
    return true;
  }

};

struct VNCServerSpawnXS : public VNCServerSpawnXBase
{
  VNCServerSpawnXS(const char* name_) : VNCServerSpawnXBase(name_) { }

  std::list<XDesktop*> displays;

  virtual std::shared_ptr<VNCScreenSpawn> createVNCScreen(std::string const & userName) override
  {
    int n = 10 + displays.size();
    auto desktop = new XDesktop(n, userName);
    displays.push_back(desktop);
    return desktop;
  }


};

char* programName;

static void printVersion(FILE *fp)
{
  fprintf(fp, "TigerVNC Server version %s, built %s\n",
          PACKAGE_VERSION, buildtime);
}

static void usage()
{
  printVersion(stderr);
  fprintf(stderr, "\nUsage: %s [<parameters>]\n", programName);
  fprintf(stderr, "       %s --version\n", programName);
  fprintf(stderr,"\n"
          "Parameters can be turned on with -<param> or off with -<param>=0\n"
          "Parameters which take a value can be specified as "
          "-<param> <value>\n"
          "Other valid forms are <param>=<value> -<param>=<value> "
          "--<param>=<value>\n"
          "Parameter names are case-insensitive.  The parameters are:\n\n");
  Configuration::listParams(79, 14);
  exit(1);
}

int main(int argc, char** argv)
{
  initStdIOLoggers();
  LogWriter::setLogParams("*:stderr:30");

  programName = argv[0];

  Configuration::enableServerParams();

  for (int i = 1; i < argc; i++) {
    if (Configuration::setParam(argv[i]))
      continue;

    if (argv[i][0] == '-') {
      if (i+1 < argc) {
        if (Configuration::setParam(&argv[i][1], argv[i+1])) {
          i++;
          continue;
        }
      }
      if (strcmp(argv[i], "-v") == 0 ||
          strcmp(argv[i], "-version") == 0 ||
          strcmp(argv[i], "--version") == 0) {
        printVersion(stdout);
        return 0;
      }
      usage();
    }

    usage();
  }

  signal(SIGHUP, CleanupSignalHandler);
  signal(SIGINT, CleanupSignalHandler);
  signal(SIGTERM, CleanupSignalHandler);

  std::list<SocketListener*> listeners;
  VNCServerSpawnXS server("spawnvncserver");

  try {

    if (rfbunixpath.getValueStr()[0] != '\0') {
      listeners.push_back(new network::UnixListener(rfbunixpath, rfbunixmode));
      vlog.info("Listening on %s (mode %04o)", (const char*)rfbunixpath, (int)rfbunixmode);
    } else {
      if (localhostOnly)
        createLocalTcpListeners(&listeners, (int)rfbport);
      else
        createTcpListeners(&listeners, 0, (int)rfbport);
      vlog.info("Listening on port %d", (int)rfbport);
    }

    const char *hostsData = hostsFile.getData();
    FileTcpFilter fileTcpFilter(hostsData);
    if (strlen(hostsData) != 0)
      for (std::list<SocketListener*>::iterator i = listeners.begin();
           i != listeners.end();
           i++)
        (*i)->setFilter(&fileTcpFilter);
    delete[] hostsData;

    while (!caughtSignal) {
      int wait_ms;
      struct timeval tv;
      fd_set rfds, wfds;
      std::list<Socket*> sockets;
      std::list<Socket*>::iterator i;

      // Process any incoming X events
      for(auto &x: server.displays) {
        x->processPendingXEvent();
      }

      FD_ZERO(&rfds);
      FD_ZERO(&wfds);

      for (auto &x: server.displays) {
        FD_SET(x->getFd(), &rfds);
      }
      for (std::list<SocketListener*>::iterator i = listeners.begin();
           i != listeners.end();
           i++)
        FD_SET((*i)->getFd(), &rfds);

      server.getSockets(&sockets);
      int clients_connected = 0;
      for (i = sockets.begin(); i != sockets.end(); i++) {
        if ((*i)->isShutdown()) {
          server.removeSocket(*i);
          delete (*i);
        } else {
          FD_SET((*i)->getFd(), &rfds);
          if ((*i)->outStream().bufferUsage() > 0)
            FD_SET((*i)->getFd(), &wfds);
          clients_connected++;
        }
      }

      wait_ms = 500;

      soonestTimeout(&wait_ms, Timer::checkTimeouts());

      tv.tv_sec = wait_ms / 1000;
      tv.tv_usec = (wait_ms % 1000) * 1000;

      // Do the wait...
      int n = select(FD_SETSIZE, &rfds, &wfds, 0,
                     wait_ms ? &tv : NULL);

      if (n < 0) {
        if (errno == EINTR) {
          vlog.debug("Interrupted select() system call");
          continue;
        } else {
          throw rdr::SystemException("select", errno);
        }
      }

      // Accept new VNC connections
      for (std::list<SocketListener*>::iterator i = listeners.begin();
           i != listeners.end();
           i++) {
        if (FD_ISSET((*i)->getFd(), &rfds)) {
          Socket* sock = (*i)->accept();
          if (sock) {
            sock->outStream().setBlocking(false);
            server.addSocket(sock);
          } else {
            vlog.status("Client connection rejected");
          }
        }
      }

      Timer::checkTimeouts();

      // Client list could have been changed.
      server.getSockets(&sockets);

      // Nothing more to do if there are no client connections.
      if (sockets.empty())
        continue;

      // Process events on existing VNC connections
      for (i = sockets.begin(); i != sockets.end(); i++) {
        if (FD_ISSET((*i)->getFd(), &rfds))
          server.processSocketReadEvent(*i);
        if (FD_ISSET((*i)->getFd(), &wfds))
          server.processSocketWriteEvent(*i);
      }

    }

  } catch (rdr::Exception &e) {
    vlog.error("%s", e.str());
    return 1;
  }

  for(auto &x: server.displays) {
    x->processPendingXEvent();
  }

  // Run listener destructors; remove UNIX sockets etc
  for (std::list<SocketListener*>::iterator i = listeners.begin();
       i != listeners.end();
       i++) {
    delete *i;
  }

  vlog.info("Terminated");
  return 0;
}
