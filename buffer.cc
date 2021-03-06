// CODYlib		-*- mode:c++ -*-
// Copyright (C) 2020 Nathan Sidwell, nathan@acm.org
// License: Apache v2.0

// Cody
#include "internal.hh"
// C++
#include <algorithm>
// C
#include <cstring>
// OS
#include <unistd.h>
#include <cerrno>

// MessageBuffer code

// Lines consist of words and end with a NEWLINE (0xa) char
// Whitespace characters are TAB (0x9) and SPACE (0x20)
// Words consist of non-whitespace chars separated by whitespace.
// Multiple lines in one transaction are indicated by ending non-final
// lines with a SEMICOLON (0x3b) word, immediately before the NEWLINE
// Continuations with ; preceding it
// Words matching regexp [-+_/%.a-zA-Z0-9]+ need no quoting.
// Quoting with '...'
// Anything outside of [-+_/%.a-zA-Z0-9] needs quoting
// Anything outside of <= <space> or DEL or \' or \\ needs escaping.
// Escapes are \\, \', \n, \t, \_, everything else as \<hex><hex>?
// Spaces separate words, UTF8 encoding for non-ascii chars

namespace Cody {
namespace Detail {

static const char CONTINUE = ';';

void MessageBuffer::BeginLine ()
{
  if (!buffer.empty ())
    {
      // Terminate the previous line with a continuation
      buffer.reserve (buffer.size () + 3);
      buffer.push_back (' ');
      buffer.push_back (CONTINUE);
      buffer.push_back ('\n');
    }
  lastBol = buffer.size ();
}

// QUOTE means 'maybe quote', we search it for quote-needing chars

void MessageBuffer::Append (char const *str, bool quote, size_t len)
{
  if (len == ~size_t (0))
    len = strlen (str);

  if (!len && !quote)
    return;

  // We want to quote characters outside of [-+_A-Za-z0-9/%.], anything
  // that could remotely be shell-active.  UTF8 encoding for non-ascii.
  if (quote && len)
    {
      quote = false;
      // Scan looking for quote-needing characters.  We could just
      // append until we find one, but that's probably confusing
      for (size_t ix = len; ix--;)
	{
	  unsigned char c = (unsigned char)str[ix];
	  if (!((c >= 'a' && c <= 'z')
		|| (c >= 'A' && c <= 'Z')
		|| (c >= '0' && c <= '9')
		|| c == '-' || c == '+' || c == '_'
		|| c == '/' || c == '%' || c == '.'))
	    {
	      quote = true;
	      break;
	    }
	}
    }

  // Maximal length of appended string
  buffer.reserve (buffer.size () + len * (quote ? 3 : 1) + 2);

  if (quote)
    buffer.push_back ('\'');

  for (auto *end = str + len; str != end;)
    {
      auto *e = end;

      if (quote)
	// Look for next escape-needing char.  More relaxed than
	// the earlier needs-quoting check.
	for (e = str; e != end; ++e)
	  {
	    unsigned char c = (unsigned char)*e;
	    if (c < ' ' || c == 0x7f || c == '\\' || c == '\'')
	      break;
	  }
      buffer.insert (buffer.end (), str, e);
      str = e;

      if (str == end)
	break;

      buffer.push_back ('\\');
      switch (unsigned char c = (unsigned char)*str++)
	{
	case '\t':
	  c = 't';
	  goto append;

	case '\n':
	  c = 'n';
	  goto append;

	case '\'':
	case '\\':
	append:
	  buffer.push_back (c);
	  break;

	default:
	  // Full-on escape.  Use 2 lower-case hex chars
	  for (unsigned shift = 8; shift;)
	    {
	      shift -= 4;

	      char nibble = (c >> shift) & 0xf;
	      nibble += '0';
	      if (nibble > '9')
		nibble += 'a' - ('9' + 1);
	      buffer.push_back (nibble);
	    }
	}
    }

  if (quote)
    buffer.push_back ('\'');  
}

void MessageBuffer::Append (char c)
{
  buffer.push_back (c);
}

void MessageBuffer::AppendInteger (unsigned u)
{
  std::string v (std::to_string (u));
  AppendWord (v);
}

int MessageBuffer::Write (int fd) noexcept
{
  size_t limit = buffer.size () - lastBol;
  ssize_t count = write (fd, &buffer.data ()[lastBol], limit);

  int err = 0;
  if (count < 0)
    err = errno;
  else
    {
      lastBol += count;
      if (size_t (count) != limit)
	err = EAGAIN;
    }

  if (err != EAGAIN && err != EINTR)
    {
      // Reset for next message
      buffer.clear ();
      lastBol = 0;
    }

  return err;
}

int MessageBuffer::Read (int fd) noexcept
{
  constexpr size_t blockSize = 200;

  size_t lwm = buffer.size ();
  size_t hwm = buffer.capacity ();
  if (hwm - lwm < blockSize / 2)
    hwm += blockSize;
  buffer.resize (hwm);

  auto iter = buffer.begin () + lwm;
  ssize_t count = read (fd, &*iter, hwm - lwm);
  buffer.resize (lwm + (count >= 0 ? count : 0));

  if (count < 0)
    return errno;

  if (!count)
    // End of file
    return -1;

  bool more = true;
  for (;;)
    {
      auto newline = std::find (iter, buffer.end (), '\n');
      if (newline == buffer.end ())
	break;
      more = newline != buffer.begin () && newline[-1] == CONTINUE;
      iter = newline + 1;
	
      if (iter == buffer.end ())
	break;

      if (!more)
	{
	  // There is no continuation, but there are chars after the
	  // newline.  Truncate the buffer and return an error
	  buffer.resize (iter - buffer.begin ());
	  return EINVAL;
	}
    }

  return more ? EAGAIN : 0;
}

int MessageBuffer::Lex (std::vector<std::string> &result)
{
  result.clear ();

  int err = ENOMSG;
  if (IsAtEnd ())
    return ENOMSG;

  Assert (buffer.back () == '\n');

  auto iter = buffer.begin () + lastBol;

  for (std::string *word = nullptr;;)
    {
      char c = *iter;

      ++iter;
      if (c == ' ' || c == '\t')
	{
	  word = nullptr;
	  continue;
	}

      if (c == '\n')
	break;

      if (c == CONTINUE)
	{
	  // Line continuation
	  if (word || *iter != '\n')
	    goto malformed;
	  ++iter;
	  break;
	}

      if (c <= ' ' || c >= 0x7f)
	goto malformed;

      if (!word)
	{
	  result.emplace_back ();
	  word = &result.back ();
	}

      if (c == '\'')
	{
	  // Quoted word
	  for (;;)
	    {
	      c = *iter;

	      if (c == '\n')
		{
		malformed:;
		  result.clear ();
		  iter = std::find (iter, buffer.end (), '\n');
		  auto back = iter;
		  if (back[-1] == CONTINUE  && back[-2] == ' ')
		    // Smells like a line continuation
		    back -= 2;
		  result.emplace_back (&buffer[lastBol],
				       back - buffer.begin () - lastBol);
		  ++iter;
		  lastBol = iter - buffer.begin ();
		  return EINVAL;
		}

	      if (c < ' ' || c >= 0x7f)
		goto malformed;

	      ++iter;
	      if (c == '\'')
		break;

	      if (c == '\\')
		// escape
		switch (c = *iter)
		  {
		    case '\\':
		    case '\'':
		      ++iter;
		      break;

		    case 'n':
		      c = '\n';
		      ++iter;
		      break;

		    case '_':
		      // We used to escape SPACE as \_, so accept that
		      c = ' ';
		      ++iter;
		      break;

		    case 't':
		      c = '\t';
		      ++iter;
		      break;

		    default:
		      {
			unsigned v = 0;
			for (unsigned nibble = 0; nibble != 2; nibble++)
			  {
			    c = *iter;
			    if (c < '0')
			      {
				if (!nibble)
				  goto malformed;
				break;
			      }
			    else if (c <= '9')
			      c -= '0';
			    else if (c < 'a')
			      {
				if (!nibble)
				  goto malformed;
				break;
			      }
			    else if (c <= 'f')
			      c -= 'a' - 10;
			    else
			      {
				if (!nibble)
				  goto malformed;
				break;
			      }
			    ++iter;
			    v = (v << 4) | c;
			  }
			c = v;
		      }
		  }
	      word->push_back (c);
	    }
	}
      else
	// Unquoted character
	word->push_back (c);
    }
  lastBol = iter - buffer.begin ();
  if (result.empty ())
    return ENOMSG;

  return 0;
}

void MessageBuffer::LexedLine (std::string &str)
{
  if (lastBol)
    {
      size_t pos = lastBol - 1;
      for (; pos; pos--)
	if (buffer[pos-1] == '\n')
	  break;

      size_t end = lastBol - 1;
      if (buffer[end-1] == CONTINUE && buffer[end-2] == ' ')
	// Strip line continuation
	end -= 2;
      str.append (&buffer[pos], end - pos);
    }
}
} // Detail
} // Cody
