// CODYlib		-*- mode:c++ -*-
// Copyright (C) 2020 Nathan Sidwell, nathan@acm.org
// License: Apache v2.0

// Cody
#include "internal.hh"
// OS
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

// Resolver code

#if __windows__
inline bool IsDirSep (char c)
{
  return c == '/' || c == '\\';
}
inline bool IsAbsPath (char const *str)
{
  // IIRC windows has the concept of per-drive current directories,
  // which make drive-using paths confusing.  Let's not get into that.
  return IsDirSep (str)
    || (((str[0] >= 'A' && str[1] <= 'Z')
	 || (str[0] >= 'a' && str[1] <= 'z'))&& str[1] == ':');
}
#else
inline bool IsDirSep (char c)
{
  return c == '/';
}
inline bool IsAbsPath (char const *str)
{
  return IsDirSep (str[0]);
}
#endif

constexpr char DOT_REPLACE = ','; // Replace . directories
constexpr char COLON_REPLACE = '-'; // Replace : (partition char) 
constexpr char const REPO_DIR[] = "cmi.cache";

namespace Cody {

Resolver::~Resolver ()
{
}

char const *Resolver::GetCMISuffix ()
{
  return "cmi";
}

std::string Resolver::GetCMIName (std::string const &module)
{
  std::string result;

  result.reserve (module.size () + 8);
  bool is_header = false;
  bool is_abs = false;

  if (IsAbsPath (module.c_str ()))
    is_header = is_abs = true;
  else if (module.front () == '.' && IsDirSep (module.c_str ()[1]))
    is_header = true;

  if (is_abs)
    {
      result.push_back ('.');
      result.append (module);
    }
  else
    result = std::move (module);

  if (is_header)
    {
      if (!is_abs)
	result[0] = DOT_REPLACE;
      
      /* Map .. to DOT_REPLACE, DOT_REPLACE.  */
      for (size_t ix = 1; ; ix++)
	{
	  ix = result.find ('.', ix);
	  if (ix == result.npos)
	    break;
	  if (ix + 2 > result.size ())
	    break;
	  if (result[ix + 1] != '.')
	    continue;
	  if (!IsDirSep (result[ix - 1]))
	    continue;
	  if (!IsDirSep (result[ix + 2]))
	    continue;
	  result[ix] = DOT_REPLACE;
	  result[ix + 1] = DOT_REPLACE;
	}
    }
  else if (COLON_REPLACE != ':')
    {
      // There can only be one colon in a module name
      auto colon = result.find (':');
      if (colon != result.npos)
	result[colon] = COLON_REPLACE;
    }

  if (char const *suffix = GetCMISuffix ())
    {
      result.push_back ('.');
      result.append (suffix);
    }

  return result;
}

void Resolver::WaitUntilReady (Server *)
{
}

Resolver *Resolver::ConnectRequest (Server *s, unsigned version,
			       std::string &, std::string &)
{
  if (version > Version)
    s->ErrorResponse ("version mismatch");
  else
    s->ConnectResponse ("default");

  return this;
}

int Resolver::ModuleRepoRequest (Server *s)
{
  s->ModuleRepoResponse (REPO_DIR);
  return 0;
}

int Resolver::ModuleExportRequest (Server *s, std::string &module)
{
  auto cmi = GetCMIName (module);
  s->ModuleCMIResponse (cmi);
  return 0;
}

int Resolver::ModuleImportRequest (Server *s, std::string &module)
{
  auto cmi = GetCMIName (module);
  s->ModuleCMIResponse (cmi);
  return 0;
}

int Resolver::ModuleCompiledRequest (Server *s, std::string &)
{
  s->OKResponse ();
  return 0;
}

int Resolver::IncludeTranslateRequest (Server *s, std::string &include)
{
  bool xlate = false;

  // This is not the most efficient
  int fd_dir = open (REPO_DIR, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (fd_dir >= 0)
    {
      auto cmi = GetCMIName (include);
      struct stat statbuf;
      if (fstatat (fd_dir, cmi.c_str (), &statbuf, 0) == 0
	  && S_ISREG (statbuf.st_mode))
	{
	  // Sadly can't easily check if this proces has read access,
	  // except by trying to open it.
	  s->ModuleCMIResponse (cmi);
	  xlate = true;
	}
      close (fd_dir);
    }

  if (!xlate)
    s->IncludeTranslateResponse (0);

  return 0;
}

void Resolver::ErrorResponse (Server *server, std::string &&msg)
{
  server->ErrorResponse (msg);
}

}
