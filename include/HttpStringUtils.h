/*
 * SPDX-FileCopyrightText: 2019 Mike Dunston (atanisoft)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// @file std::string utility methods.

#ifndef STRINGUTILS_H_
#define STRINGUTILS_H_

#include <algorithm>
#include <string>
#include <vector>
#include <utils/StringPrintf.hxx>

namespace http
{

using std::string;
using std::vector;
using std::pair;
using std::make_pair;

/// Helper method to break a string into a pair<string, string> based on a delimiter.
///
/// @param str is the string to break.
/// @param delimiter is the delimiter to break the string on.
///
/// @return the pair of string objects.
static inline pair<string, string> break_string(string &str,
                                                const string& delimiter)
{
  size_t pos = str.find_first_of(delimiter);
  if (pos == string::npos)
  {
    return make_pair(str, "");
  }
  size_t end_pos = pos + delimiter.length();
  if (end_pos > str.length())
  {
    end_pos = pos;
  }
  return make_pair(str.substr(0, pos), str.substr(end_pos));
}

/// Helper which will break a string into multiple pieces based on a provided
/// delimiter.
///
/// @param str is the string to tokenize.
/// @param tokens is the container which will receive the tokenized strings.
/// @param delimiter to tokenize the string on.
/// @param keep_incomplete will take the last token of the input string and
/// insert it to the container as the last element when this is true. When
/// this is false the last token will not be inserted to the container.
/// @param discard_empty will discard empty tokens when set to true.
template <class ContainerT>
string::size_type tokenize(const string& str, ContainerT& tokens,
                           const string& delimiter = " ",
                           bool keep_incomplete = true,
                           bool discard_empty = false)
{
  string::size_type pos, lastPos = 0;

  using value_type = typename ContainerT::value_type;
  using size_type  = typename ContainerT::size_type;

  while(lastPos < str.length())
  {
    pos = str.find_first_of(delimiter, lastPos);
    if (pos == std::string::npos)
    {
      if (!keep_incomplete)
      {
        return lastPos;
      }
      pos = str.length();
    }

    if (pos != lastPos || !discard_empty)
    {
      tokens.emplace_back(value_type(str.data() + lastPos,
                                    (size_type)pos - lastPos));
    }
    lastPos = pos + delimiter.length();
  }
  return lastPos;
}

/// Helper which joins a vector<string> with a delimiter.
///
/// @param strings is the vector<string> to join
/// @param delimiter is the string to join the segments with.
/// @return the joined string.
static inline string string_join(const vector<string>& strings,
                                 const string& delimiter = "")
{
  string result;
  for (auto piece : strings)
  {
    if (!result.empty())
    {
      result += delimiter;
    }
    result += piece;
  }
  return result;
}

/// Helper which joins a vector<string> using a first and last iterator.
///
/// @param first is the starting iterator position.
/// @param last is the starting iterator position.
/// @param delimiter is the string to join the segments with.
/// @return the joined string.
static inline string string_join(const vector<string>::iterator first,
                                 const vector<string>::iterator last,
                                 const string& delimiter = "")
{
  vector<string> vec(first, last);
  return string_join(vec, delimiter);
}

/// Helper which URL decodes a string as described in RFC-1738 sec. 2.2.
///
/// @param source is the string to be decoded.
/// @return the decoded string.
/// RFC: https://www.ietf.org/rfc/rfc1738.txt
static inline string url_decode(const string source)
{
  string decoded = source;

  // replace + with space
  std::replace(decoded.begin(), decoded.end(), '+', ' ');

  // search and replace %{hex}{hex} with hex decoded character
  while (decoded.find("%") != string::npos)
  {
    // find the % character
    auto pos = decoded.find("%");
    if (pos + 2 < decoded.size())
    {
      // decode the character
      auto sub = decoded.substr(pos + 1, 2);
      char ch = std::stoi(sub, nullptr, 16);
      // insert the replacement
      decoded.insert(pos, 1, ch);
      // remove the decoded piece
      decoded.erase(pos + 1, 3);
    }
    else
    {
      // the % is not followed by at least two characters.
      break;
    }
  }
  return decoded;
}

/// Helper which URL encodes a string as described in RFC-1738 sec. 2.2.
///
/// @param source is the string to be encoded.
/// @return the encoded string.
///
/// NOTE: this method does not take into account the encoding of a URI with
/// query parameters after the "?". This should be handled by the caller by
/// passing the path and query portions separately at this time.
/// RFC: https://www.ietf.org/rfc/rfc1738.txt
static inline string url_encode(const string source)
{
  const string reserved_characters = "?#/:;+@&=";
  const string illegal_characters = "%<>{}|\\\"^`!*'()$,[]";
  string encoded = "";

  // reserve the size of the source string plus 25%, this space will be
  // reclaimed if the final string length is shorter.
  encoded.reserve(source.length() + (source.length() / 4));

  // process the source string character by character checking for any that
  // are outside the ASCII printable character range, in the reserve character
  // list or illegal character list. For accepted characters it will be added
  // to the encoded string directly, any that require encoding will be hex
  // encoded before being added to the encoded string.
  for (auto ch : source)
  {
    if (ch <= 0x20 || ch >= 0x7F ||
        reserved_characters.find(ch) != string::npos ||
        illegal_characters.find(ch) != string::npos)
    {
      // if it is outside the printable ASCII character *OR* is in either the
      // reserved or illegal characters we need to encode it as %HH.
      // NOTE: space will be encoded as "%20" and not as "+", either is an
      // acceptable option per the RFC.
      encoded += StringPrintf("%%%02x", ch);
    }
    else
    {
      encoded += ch;
    }
  }
  // shrink the buffer to the actual length
  encoded.shrink_to_fit();
  return encoded;
}

} // namespace http

#endif // STRINGUTILS_H_