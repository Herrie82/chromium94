// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "net/cookies/cookie_constants.h"
#include "net/cookies/parsed_cookie.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

TEST(ParsedCookieTest, TestBasic) {
  ParsedCookie pc1("a=b");
  EXPECT_TRUE(pc1.IsValid());
  EXPECT_FALSE(pc1.IsSecure());
  EXPECT_FALSE(pc1.IsHttpOnly());
  EXPECT_FALSE(pc1.IsSameParty());
  EXPECT_FALSE(pc1.IsPartitioned());
  EXPECT_EQ("a", pc1.Name());
  EXPECT_EQ("b", pc1.Value());
  EXPECT_FALSE(pc1.HasPath());
  EXPECT_FALSE(pc1.HasDomain());
  EXPECT_FALSE(pc1.HasExpires());
  EXPECT_FALSE(pc1.HasMaxAge());
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc1.SameSite());
  EXPECT_EQ(CookiePriority::COOKIE_PRIORITY_DEFAULT, pc1.Priority());

  ParsedCookie pc2(
      "c=d; secure; httponly; sameparty; path=/foo; domain=bar.test; "
      "max-age=60; samesite=lax; priority=high; partitioned;");
  EXPECT_TRUE(pc2.IsValid());
  EXPECT_TRUE(pc2.IsSecure());
  EXPECT_TRUE(pc2.IsHttpOnly());
  EXPECT_TRUE(pc2.IsSameParty());
  EXPECT_TRUE(pc2.IsPartitioned());
  EXPECT_EQ("c", pc2.Name());
  EXPECT_EQ("d", pc2.Value());
  EXPECT_TRUE(pc2.HasPath());
  EXPECT_EQ("/foo", pc2.Path());
  EXPECT_TRUE(pc2.HasDomain());
  EXPECT_EQ("bar.test", pc2.Domain());
  EXPECT_FALSE(pc2.HasExpires());
  EXPECT_TRUE(pc2.HasMaxAge());
  EXPECT_EQ("60", pc2.MaxAge());
  EXPECT_EQ(CookieSameSite::LAX_MODE, pc2.SameSite());
  EXPECT_EQ(CookiePriority::COOKIE_PRIORITY_HIGH, pc2.Priority());
}

TEST(ParsedCookieTest, TestEmpty) {
  const char* kTestCookieLines[]{"",    "     ", "=",     "=;",  " =;",
                                 "= ;", " = ;",  ";",     " ;",  " ; ",
                                 "\t",  "\t;",   "\t=\t", "\t=", "=\t"};

  for (const char* test : kTestCookieLines) {
    ParsedCookie pc(test);
    EXPECT_FALSE(pc.IsValid());
  }
}

TEST(ParsedCookieTest, TestSetEmptyNameValue) {
  ParsedCookie empty("");
  EXPECT_FALSE(empty.IsValid());
  EXPECT_FALSE(empty.SetName(""));
  EXPECT_FALSE(empty.SetValue(""));
  EXPECT_FALSE(empty.IsValid());

  ParsedCookie empty_value("name=");
  EXPECT_TRUE(empty_value.IsValid());
  EXPECT_EQ("name", empty_value.Name());
  EXPECT_FALSE(empty_value.SetName(""));
  EXPECT_EQ("name", empty_value.Name());
  EXPECT_TRUE(empty_value.IsValid());

  ParsedCookie empty_name("value");
  EXPECT_TRUE(empty_name.IsValid());
  EXPECT_EQ("value", empty_name.Value());
  EXPECT_FALSE(empty_name.SetValue(""));
  EXPECT_EQ("value", empty_name.Value());
  EXPECT_TRUE(empty_name.IsValid());
}

TEST(ParsedCookieTest, ParseValueStrings) {
  std::string valid_values[] = {
      "httpONLY", "1%7C1624663551161", "<K0<r<C_<G_<S0",
      "lastRequest=1624663552846&activeDays=%5B0%2C0", "si=8da88dce-5fee-4835"};
  for (const auto& value : valid_values) {
    EXPECT_EQ(ParsedCookie::ParseValueString(value), value);
    EXPECT_TRUE(ParsedCookie::ValueMatchesParsedValue(value));
  }

  std::string invalid_values[] = {
      "\nhttpONLYsecure",            // Newline char at start
      "httpONLY\nsecure",            // Newline char in middle
      "httpONLYsecure\n",            // Newline char at end
      "\r<K0<r<C_<G_<S0",            // Carriage return at start
      "<K0<r\r<C_<G_<S0",            // Carriage return in middle
      "<K0<r<C_<G_<S0\r",            // Carriage return at end
      ";lastRequest=1624663552846",  // Token separator at start
      "lastRequest=1624663552846; activeDays=%5B0%2C0",  // Token separator in
                                                         // middle
      std::string("\0abcdef", 7),                        // 0 byte at start
      std::string("abc\0def", 7),                        // 0 byte in middle
      std::string("abcdef\0", 7)};                       // 0 byte at end
  for (const auto& value : invalid_values) {
    EXPECT_NE(ParsedCookie::ParseValueString(value), value);
    EXPECT_FALSE(ParsedCookie::ValueMatchesParsedValue(value));
  }

  // Strings with leading whitespace should parse OK but
  // ValueMatchesParsedValue() should fail.
  std::string leading_whitespace_values[] = {
      " 1%7C1624663551161",   // Space at start
      "\t1%7C1624663551161",  // Tab at start
  };
  for (const auto& value : leading_whitespace_values) {
    EXPECT_TRUE(ParsedCookie::ParseValueString(value).length() ==
                value.length() - 1);
    EXPECT_FALSE(ParsedCookie::ValueMatchesParsedValue(value));
  }

  // Strings with trailing whitespace or the separator character should parse OK
  // but ValueMatchesParsedValue() should fail.
  std::string valid_values_with_trailing_chars[] = {
      "lastRequest=1624663552846 ",   // Space at end
      "lastRequest=1624663552846\t",  // Tab at end
      "lastRequest=1624663552846;",   // Token separator at end
  };
  const size_t valid_value_length =
      valid_values_with_trailing_chars[0].length() - 1;
  for (const auto& value : valid_values_with_trailing_chars) {
    EXPECT_TRUE(ParsedCookie::ParseValueString(value).length() ==
                valid_value_length);
    EXPECT_FALSE(ParsedCookie::ValueMatchesParsedValue(value));
  }

  // A valid value (truncated after the ';') but parses out to a substring.
  std::string value_with_separator_in_middle(
      "lastRequest=1624663552846; activeDays=%5B0%2C0");
  EXPECT_TRUE(
      ParsedCookie::ParseValueString(value_with_separator_in_middle).length() ==
      value_with_separator_in_middle.find(';'));
  EXPECT_FALSE(
      ParsedCookie::ValueMatchesParsedValue(value_with_separator_in_middle));
}

TEST(ParsedCookieTest, TestQuoted) {
  // These are some quoting cases which the major browsers all
  // handle differently.  I've tested Internet Explorer 6, Opera 9.6,
  // Firefox 3, and Safari Windows 3.2.1.  We originally tried to match
  // Firefox closely, however we now match Internet Explorer and Safari.
  const struct {
    const char* input;
    const char* expected;
  } kTests[] = {
      // Trailing whitespace after a quoted value.  The whitespace after
      // the quote is stripped in all browsers.
      {"\"zzz \"  ", "\"zzz \""},
      // Handling a quoted value with a ';', like FOO="zz;pp"  ;
      // IE and Safari: "zz;
      // Firefox and Opera: "zz;pp"
      {"\"zz;pp\" ;", "\"zz"},
      // Handling a value with multiple quoted parts, like FOO="zzz "   "ppp" ;
      // IE and Safari: "zzz "   "ppp";
      // Firefox: "zzz ";
      // Opera: <rejects cookie>
      {
          "\"zzz \"   \"ppp\" ", "\"zzz \"   \"ppp\"",
      },
      // A quote in a value that didn't start quoted.  like FOO=A"B ;
      // IE, Safari, and Firefox: A"B;
      // Opera: <rejects cookie>
      {
          "A\"B", "A\"B",
      }};

  for (const auto& test : kTests) {
    ParsedCookie pc(std::string("aBc=") + test.input +
                    " ; path=\"/\"  ; httponly ");
    EXPECT_TRUE(pc.IsValid());
    EXPECT_FALSE(pc.IsSecure());
    EXPECT_TRUE(pc.IsHttpOnly());
    EXPECT_TRUE(pc.HasPath());
    EXPECT_EQ("aBc", pc.Name());
    EXPECT_EQ(test.expected, pc.Value());

    // If a path was quoted, the path attribute keeps the quotes.  This will
    // make the cookie effectively useless, but path parameters aren't supposed
    // to be quoted.  Bug 1261605.
    EXPECT_EQ("\"/\"", pc.Path());
  }
}

TEST(ParsedCookieTest, TestNameless) {
  ParsedCookie pc("BLAHHH; path=/; secure;");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_TRUE(pc.IsSecure());
  EXPECT_TRUE(pc.HasPath());
  EXPECT_EQ("/", pc.Path());
  EXPECT_EQ("", pc.Name());
  EXPECT_EQ("BLAHHH", pc.Value());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
}

TEST(ParsedCookieTest, TestAttributeCase) {
  ParsedCookie pc(
      "BLAH; Path=/; sECuRe; httpONLY; sAmESitE=LaX; pRIoRitY=hIgH; samePaRtY; "
      "pARTitIoNeD;");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_TRUE(pc.IsSecure());
  EXPECT_TRUE(pc.IsHttpOnly());
  EXPECT_TRUE(pc.IsSameParty());
  EXPECT_TRUE(pc.IsPartitioned());
  EXPECT_EQ(CookieSameSite::LAX_MODE, pc.SameSite());
  EXPECT_TRUE(pc.HasPath());
  EXPECT_EQ("/", pc.Path());
  EXPECT_EQ("", pc.Name());
  EXPECT_EQ("BLAH", pc.Value());
  EXPECT_EQ(COOKIE_PRIORITY_HIGH, pc.Priority());
  EXPECT_EQ(7U, pc.NumberOfAttributes());
}

TEST(ParsedCookieTest, TestDoubleQuotedNameless) {
  ParsedCookie pc("\"BLA\\\"HHH\"; path=/; secure;");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_TRUE(pc.IsSecure());
  EXPECT_TRUE(pc.HasPath());
  EXPECT_EQ("/", pc.Path());
  EXPECT_EQ("", pc.Name());
  EXPECT_EQ("\"BLA\\\"HHH\"", pc.Value());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
  EXPECT_EQ(2U, pc.NumberOfAttributes());
}

TEST(ParsedCookieTest, QuoteOffTheEnd) {
  ParsedCookie pc("a=\"B");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_EQ("a", pc.Name());
  EXPECT_EQ("\"B", pc.Value());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
  EXPECT_EQ(0U, pc.NumberOfAttributes());
}

TEST(ParsedCookieTest, MissingName) {
  ParsedCookie pc("=ABC");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_EQ("", pc.Name());
  EXPECT_EQ("ABC", pc.Value());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
  EXPECT_EQ(0U, pc.NumberOfAttributes());
}

TEST(ParsedCookieTest, MissingValue) {
  ParsedCookie pc("ABC=;  path = /wee");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_EQ("ABC", pc.Name());
  EXPECT_EQ("", pc.Value());
  EXPECT_TRUE(pc.HasPath());
  EXPECT_EQ("/wee", pc.Path());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
  EXPECT_EQ(1U, pc.NumberOfAttributes());
}

TEST(ParsedCookieTest, Whitespace) {
  ParsedCookie pc("  A  = BC  ;secure;;;   samesite = lax     ");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_EQ("A", pc.Name());
  EXPECT_EQ("BC", pc.Value());
  EXPECT_FALSE(pc.HasPath());
  EXPECT_FALSE(pc.HasDomain());
  EXPECT_TRUE(pc.IsSecure());
  EXPECT_FALSE(pc.IsHttpOnly());
  EXPECT_EQ(CookieSameSite::LAX_MODE, pc.SameSite());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
  // We parse anything between ; as attributes, so we end up with two
  // attributes with an empty string name and value.
  EXPECT_EQ(4U, pc.NumberOfAttributes());
}
TEST(ParsedCookieTest, MultipleEquals) {
  ParsedCookie pc("  A=== BC  ;secure;;;   httponly");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_EQ("A", pc.Name());
  EXPECT_EQ("== BC", pc.Value());
  EXPECT_FALSE(pc.HasPath());
  EXPECT_FALSE(pc.HasDomain());
  EXPECT_TRUE(pc.IsSecure());
  EXPECT_TRUE(pc.IsHttpOnly());
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc.SameSite());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
  EXPECT_EQ(4U, pc.NumberOfAttributes());
}

TEST(ParsedCookieTest, QuotedTrailingWhitespace) {
  ParsedCookie pc(
      "ANCUUID=\"zohNumRKgI0oxyhSsV3Z7D\"  ; "
      "expires=Sun, 18-Apr-2027 21:06:29 GMT ; "
      "path=/  ;  ");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_EQ("ANCUUID", pc.Name());
  // Stripping whitespace after the quotes matches all other major browsers.
  EXPECT_EQ("\"zohNumRKgI0oxyhSsV3Z7D\"", pc.Value());
  EXPECT_TRUE(pc.HasExpires());
  EXPECT_TRUE(pc.HasPath());
  EXPECT_EQ("/", pc.Path());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
  EXPECT_EQ(2U, pc.NumberOfAttributes());
}

TEST(ParsedCookieTest, TrailingWhitespace) {
  ParsedCookie pc(
      "ANCUUID=zohNumRKgI0oxyhSsV3Z7D  ; "
      "expires=Sun, 18-Apr-2027 21:06:29 GMT ; "
      "path=/  ;  ");
  EXPECT_TRUE(pc.IsValid());
  EXPECT_EQ("ANCUUID", pc.Name());
  EXPECT_EQ("zohNumRKgI0oxyhSsV3Z7D", pc.Value());
  EXPECT_TRUE(pc.HasExpires());
  EXPECT_TRUE(pc.HasPath());
  EXPECT_EQ("/", pc.Path());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
  EXPECT_EQ(2U, pc.NumberOfAttributes());
}

TEST(ParsedCookieTest, LotsOfPairs) {
  for (int i = 1; i < 100; i++) {
    std::string blankpairs;
    blankpairs.resize(i, ';');

    ParsedCookie c("a=b;" + blankpairs + "secure");
    EXPECT_EQ("a", c.Name());
    EXPECT_EQ("b", c.Value());
    EXPECT_TRUE(c.IsValid());
    EXPECT_TRUE(c.IsSecure());
  }
}

// TODO(erikwright): some better test cases for invalid cookies.
TEST(ParsedCookieTest, InvalidTooLong) {
  std::string maxstr;
  maxstr.resize(ParsedCookie::kMaxCookieSize, 'a');

  ParsedCookie pc1(maxstr);
  EXPECT_TRUE(pc1.IsValid());

  ParsedCookie pc2(maxstr + "A");
  EXPECT_FALSE(pc2.IsValid());
}

TEST(ParsedCookieTest, EmbeddedTerminator) {
  ParsedCookie pc1("AAA=BB\0ZYX");
  ParsedCookie pc2("AAA=BB\rZYX");
  ParsedCookie pc3("AAA=BB\nZYX");
  EXPECT_TRUE(pc1.IsValid());
  EXPECT_EQ("AAA", pc1.Name());
  EXPECT_EQ("BB", pc1.Value());
  EXPECT_TRUE(pc2.IsValid());
  EXPECT_EQ("AAA", pc2.Name());
  EXPECT_EQ("BB", pc2.Value());
  EXPECT_TRUE(pc3.IsValid());
  EXPECT_EQ("AAA", pc3.Name());
  EXPECT_EQ("BB", pc3.Value());
}

TEST(ParsedCookieTest, ParseTokensAndValues) {
  EXPECT_EQ("hello", ParsedCookie::ParseTokenString("hello\nworld"));
  EXPECT_EQ("fs!!@", ParsedCookie::ParseTokenString("fs!!@;helloworld"));
  EXPECT_EQ("hello world\tgood",
            ParsedCookie::ParseTokenString("hello world\tgood\rbye"));
  EXPECT_EQ("A", ParsedCookie::ParseTokenString("A=B=C;D=E"));
  EXPECT_EQ("hello", ParsedCookie::ParseValueString("hello\nworld"));
  EXPECT_EQ("fs!!@", ParsedCookie::ParseValueString("fs!!@;helloworld"));
  EXPECT_EQ("hello world\tgood",
            ParsedCookie::ParseValueString("hello world\tgood\rbye"));
  EXPECT_EQ("A=B=C", ParsedCookie::ParseValueString("A=B=C;D=E"));
}

TEST(ParsedCookieTest, SerializeCookieLine) {
  const char input[] =
      "ANCUUID=zohNumRKgI0oxyhSsV3Z7D  ; "
      "expires=Sun, 18-Apr-2027 21:06:29 GMT ; "
      "path=/  ;  priority=low  ;  ";
  const char output[] =
      "ANCUUID=zohNumRKgI0oxyhSsV3Z7D; "
      "expires=Sun, 18-Apr-2027 21:06:29 GMT; "
      "path=/; priority=low";
  ParsedCookie pc(input);
  EXPECT_EQ(output, pc.ToCookieLine());
}

TEST(ParsedCookieTest, SetNameAndValue) {
  ParsedCookie cookie("a=b");
  EXPECT_TRUE(cookie.IsValid());
  EXPECT_TRUE(cookie.SetDomain("foobar.com"));
  EXPECT_TRUE(cookie.SetName("name"));
  EXPECT_TRUE(cookie.SetValue("value"));
  EXPECT_EQ("name=value; domain=foobar.com", cookie.ToCookieLine());
  EXPECT_TRUE(cookie.IsValid());

  // We don't test
  //   ParsedCookie invalid("@foo=bar");
  //   EXPECT_FALSE(invalid.IsValid());
  // here because we are slightly more tolerant to invalid cookie names and
  // values that are set by webservers. We only enforce a correct name and
  // value if set via SetName() and SetValue().

  ParsedCookie pc("name=value");
  EXPECT_TRUE(pc.IsValid());

  // Set invalid name / value.
  EXPECT_FALSE(pc.SetName("@foobar"));
  EXPECT_EQ("name=value", pc.ToCookieLine());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_FALSE(pc.SetValue("foo bar"));
  EXPECT_EQ("name=value", pc.ToCookieLine());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_FALSE(pc.SetValue("\"foobar"));
  EXPECT_EQ("name=value", pc.ToCookieLine());
  EXPECT_TRUE(pc.IsValid());

  // Set valid name / value
  EXPECT_TRUE(pc.SetName(std::string()));
  EXPECT_EQ("=value", pc.ToCookieLine());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_TRUE(pc.SetName("test"));
  EXPECT_EQ("test=value", pc.ToCookieLine());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_TRUE(pc.SetValue("\"foobar\""));
  EXPECT_EQ("test=\"foobar\"", pc.ToCookieLine());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_TRUE(pc.SetValue(std::string()));
  EXPECT_EQ("test=", pc.ToCookieLine());
  EXPECT_TRUE(pc.IsValid());
}

TEST(ParsedCookieTest, SetAttributes) {
  ParsedCookie pc("name=value");
  EXPECT_TRUE(pc.IsValid());

  // Clear an unset attribute.
  EXPECT_TRUE(pc.SetDomain(std::string()));
  EXPECT_FALSE(pc.HasDomain());
  EXPECT_EQ("name=value", pc.ToCookieLine());
  EXPECT_TRUE(pc.IsValid());

  // Set a string containing an invalid character
  EXPECT_FALSE(pc.SetDomain("foo;bar"));
  EXPECT_FALSE(pc.HasDomain());
  EXPECT_EQ("name=value", pc.ToCookieLine());
  EXPECT_TRUE(pc.IsValid());

  // Set all other attributes and check that they are appended in order.
  EXPECT_TRUE(pc.SetDomain("domain.com"));
  EXPECT_TRUE(pc.SetPath("/"));
  EXPECT_TRUE(pc.SetExpires("Sun, 18-Apr-2027 21:06:29 GMT"));
  EXPECT_TRUE(pc.SetMaxAge("12345"));
  EXPECT_TRUE(pc.SetIsSecure(true));
  EXPECT_TRUE(pc.SetIsHttpOnly(true));
  EXPECT_TRUE(pc.SetIsHttpOnly(true));
  EXPECT_TRUE(pc.SetSameSite("LAX"));
  EXPECT_TRUE(pc.SetPriority("HIGH"));
  EXPECT_TRUE(pc.SetIsSameParty(true));
  EXPECT_TRUE(pc.SetIsPartitioned(true));
  EXPECT_EQ(
      "name=value; domain=domain.com; path=/; "
      "expires=Sun, 18-Apr-2027 21:06:29 GMT; max-age=12345; secure; "
      "httponly; samesite=LAX; priority=HIGH; sameparty; partitioned",
      pc.ToCookieLine());
  EXPECT_TRUE(pc.HasDomain());
  EXPECT_TRUE(pc.HasPath());
  EXPECT_TRUE(pc.HasExpires());
  EXPECT_TRUE(pc.HasMaxAge());
  EXPECT_TRUE(pc.IsSecure());
  EXPECT_TRUE(pc.IsHttpOnly());
  EXPECT_EQ(CookieSameSite::LAX_MODE, pc.SameSite());
  EXPECT_EQ(COOKIE_PRIORITY_HIGH, pc.Priority());
  EXPECT_TRUE(pc.IsSameParty());

  // Modify one attribute in the middle.
  EXPECT_TRUE(pc.SetPath("/foo"));
  EXPECT_TRUE(pc.HasDomain());
  EXPECT_TRUE(pc.HasPath());
  EXPECT_EQ("/foo", pc.Path());
  EXPECT_TRUE(pc.HasExpires());
  EXPECT_TRUE(pc.IsSecure());
  EXPECT_TRUE(pc.IsHttpOnly());
  EXPECT_TRUE(pc.IsSameParty());
  EXPECT_EQ(
      "name=value; domain=domain.com; path=/foo; "
      "expires=Sun, 18-Apr-2027 21:06:29 GMT; max-age=12345; secure; "
      "httponly; samesite=LAX; priority=HIGH; sameparty; partitioned",
      pc.ToCookieLine());

  // Set priority to medium.
  EXPECT_TRUE(pc.SetPriority("medium"));
  EXPECT_EQ(CookiePriority::COOKIE_PRIORITY_MEDIUM, pc.Priority());
  EXPECT_EQ(
      "name=value; domain=domain.com; path=/foo; "
      "expires=Sun, 18-Apr-2027 21:06:29 GMT; max-age=12345; secure; "
      "httponly; samesite=LAX; priority=medium; sameparty; partitioned",
      pc.ToCookieLine());

  // Clear attribute from the end.
  EXPECT_TRUE(pc.SetIsPartitioned(false));
  EXPECT_FALSE(pc.IsPartitioned());
  EXPECT_EQ(
      "name=value; domain=domain.com; path=/foo; "
      "expires=Sun, 18-Apr-2027 21:06:29 GMT; max-age=12345; secure; "
      "httponly; samesite=LAX; priority=medium; sameparty",
      pc.ToCookieLine());

  // Clear the rest and change the name and value.
  EXPECT_TRUE(pc.SetDomain(std::string()));
  EXPECT_TRUE(pc.SetPath(std::string()));
  EXPECT_TRUE(pc.SetExpires(std::string()));
  EXPECT_TRUE(pc.SetMaxAge(std::string()));
  EXPECT_TRUE(pc.SetIsSecure(false));
  EXPECT_TRUE(pc.SetIsHttpOnly(false));
  EXPECT_TRUE(pc.SetSameSite(std::string()));
  EXPECT_TRUE(pc.SetName("name2"));
  EXPECT_TRUE(pc.SetValue("value2"));
  EXPECT_TRUE(pc.SetPriority(std::string()));
  EXPECT_FALSE(pc.HasDomain());
  EXPECT_FALSE(pc.HasPath());
  EXPECT_FALSE(pc.HasExpires());
  EXPECT_FALSE(pc.HasMaxAge());
  EXPECT_FALSE(pc.IsSecure());
  EXPECT_FALSE(pc.IsHttpOnly());
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc.SameSite());
  EXPECT_TRUE(pc.SetIsSameParty(false));
  EXPECT_TRUE(pc.SetIsPartitioned(false));
  EXPECT_EQ("name2=value2", pc.ToCookieLine());
  EXPECT_FALSE(pc.IsSameParty());
  EXPECT_FALSE(pc.IsPartitioned());
}

// Set the domain attribute twice in a cookie line. If the second attribute's
// value is empty, it shoud be ignored.
//
// This is de facto standard behavior, per https://crbug.com/601786.
TEST(ParsedCookieTest, MultipleDomainAttributes) {
  ParsedCookie pc1("name=value; domain=foo.com; domain=bar.com");
  EXPECT_EQ("bar.com", pc1.Domain());
  ParsedCookie pc2("name=value; domain=foo.com; domain=");
  EXPECT_EQ("foo.com", pc2.Domain());
}

TEST(ParsedCookieTest, SetPriority) {
  ParsedCookie pc("name=value");
  EXPECT_TRUE(pc.IsValid());

  EXPECT_EQ("name=value", pc.ToCookieLine());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());

  // Test each priority, expect case-insensitive compare.
  EXPECT_TRUE(pc.SetPriority("high"));
  EXPECT_EQ("name=value; priority=high", pc.ToCookieLine());
  EXPECT_EQ(COOKIE_PRIORITY_HIGH, pc.Priority());

  EXPECT_TRUE(pc.SetPriority("mEDium"));
  EXPECT_EQ("name=value; priority=mEDium", pc.ToCookieLine());
  EXPECT_EQ(COOKIE_PRIORITY_MEDIUM, pc.Priority());

  EXPECT_TRUE(pc.SetPriority("LOW"));
  EXPECT_EQ("name=value; priority=LOW", pc.ToCookieLine());
  EXPECT_EQ(COOKIE_PRIORITY_LOW, pc.Priority());

  // Interpret invalid priority values as COOKIE_PRIORITY_DEFAULT.
  EXPECT_TRUE(pc.SetPriority("Blah"));
  EXPECT_EQ("name=value; priority=Blah", pc.ToCookieLine());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());

  EXPECT_TRUE(pc.SetPriority("lowerest"));
  EXPECT_EQ("name=value; priority=lowerest", pc.ToCookieLine());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());

  EXPECT_TRUE(pc.SetPriority(""));
  EXPECT_EQ("name=value", pc.ToCookieLine());
  EXPECT_EQ(COOKIE_PRIORITY_DEFAULT, pc.Priority());
}

TEST(ParsedCookieTest, SetSameSite) {
  ParsedCookie pc("name=value");
  EXPECT_TRUE(pc.IsValid());

  EXPECT_EQ("name=value", pc.ToCookieLine());
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc.SameSite());

  // Test each samesite directive, expect case-insensitive compare.
  EXPECT_TRUE(pc.SetSameSite("strict"));
  EXPECT_EQ("name=value; samesite=strict", pc.ToCookieLine());
  EXPECT_EQ(CookieSameSite::STRICT_MODE, pc.SameSite());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_TRUE(pc.SetSameSite("lAx"));
  EXPECT_EQ("name=value; samesite=lAx", pc.ToCookieLine());
  EXPECT_EQ(CookieSameSite::LAX_MODE, pc.SameSite());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_TRUE(pc.SetSameSite("LAX"));
  EXPECT_EQ("name=value; samesite=LAX", pc.ToCookieLine());
  EXPECT_EQ(CookieSameSite::LAX_MODE, pc.SameSite());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_TRUE(pc.SetSameSite("None"));
  EXPECT_EQ("name=value; samesite=None", pc.ToCookieLine());
  EXPECT_EQ(CookieSameSite::NO_RESTRICTION, pc.SameSite());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_TRUE(pc.SetSameSite("NONE"));
  EXPECT_EQ("name=value; samesite=NONE", pc.ToCookieLine());
  EXPECT_EQ(CookieSameSite::NO_RESTRICTION, pc.SameSite());
  EXPECT_TRUE(pc.IsValid());

  // Remove the SameSite attribute.
  EXPECT_TRUE(pc.SetSameSite(""));
  EXPECT_EQ("name=value", pc.ToCookieLine());
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc.SameSite());
  EXPECT_TRUE(pc.IsValid());

  EXPECT_TRUE(pc.SetSameSite("Blah"));
  EXPECT_EQ("name=value; samesite=Blah", pc.ToCookieLine());
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc.SameSite());
  EXPECT_TRUE(pc.IsValid());
}

// Test that the correct enum value is returned for the SameSite attribute
// string.
TEST(ParsedCookieTest, CookieSameSiteStringEnum) {
  ParsedCookie pc("name=value; SameSite");
  CookieSameSiteString actual = CookieSameSiteString::kLax;
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc.SameSite(&actual));
  EXPECT_EQ(CookieSameSiteString::kEmptyString, actual);

  pc.SetSameSite("Strict");
  EXPECT_EQ(CookieSameSite::STRICT_MODE, pc.SameSite(&actual));
  EXPECT_EQ(CookieSameSiteString::kStrict, actual);

  pc.SetSameSite("Lax");
  EXPECT_EQ(CookieSameSite::LAX_MODE, pc.SameSite(&actual));
  EXPECT_EQ(CookieSameSiteString::kLax, actual);

  pc.SetSameSite("None");
  EXPECT_EQ(CookieSameSite::NO_RESTRICTION, pc.SameSite(&actual));
  EXPECT_EQ(CookieSameSiteString::kNone, actual);

  pc.SetSameSite("Extended");
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc.SameSite(&actual));
  EXPECT_EQ(CookieSameSiteString::kExtended, actual);

  pc.SetSameSite("Bananas");
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc.SameSite(&actual));
  EXPECT_EQ(CookieSameSiteString::kUnrecognized, actual);

  ParsedCookie pc2("no_samesite=1");
  EXPECT_EQ(CookieSameSite::UNSPECIFIED, pc2.SameSite(&actual));
  EXPECT_EQ(CookieSameSiteString::kUnspecified, actual);
}

TEST(ParsedCookieTest, SettersInputValidation) {
  ParsedCookie pc("name=foobar");
  EXPECT_TRUE(pc.SetPath("baz"));
  EXPECT_EQ(pc.ToCookieLine(), "name=foobar; path=baz");

  EXPECT_TRUE(pc.SetPath("  baz "));
  EXPECT_EQ(pc.ToCookieLine(), "name=foobar; path=baz");

  EXPECT_TRUE(pc.SetPath("     "));
  EXPECT_EQ(pc.ToCookieLine(), "name=foobar");

  EXPECT_TRUE(pc.SetDomain("  baz "));
  EXPECT_EQ(pc.ToCookieLine(), "name=foobar; domain=baz");

  // Invalid characters
  EXPECT_FALSE(pc.SetPath("  baz\n "));
  EXPECT_FALSE(pc.SetPath("f;oo"));
  EXPECT_FALSE(pc.SetPath("\r"));
  EXPECT_FALSE(pc.SetPath("\a"));
  EXPECT_FALSE(pc.SetPath("\t"));
  EXPECT_FALSE(pc.SetSameSite("\r"));
}

TEST(ParsedCookieTest, ToCookieLineSpecialTokens) {
  // Special tokens "secure", "httponly", and "sameparty" should be treated as
  // any other name when they are in the first position.
  {
    ParsedCookie pc("");
    pc.SetName("secure");
    EXPECT_EQ(pc.ToCookieLine(), "secure=");
  }
  {
    ParsedCookie pc("secure");
    EXPECT_EQ(pc.ToCookieLine(), "=secure");
  }
  {
    ParsedCookie pc("secure=foo");
    EXPECT_EQ(pc.ToCookieLine(), "secure=foo");
  }
  {
    ParsedCookie pc("foo=secure");
    EXPECT_EQ(pc.ToCookieLine(), "foo=secure");
  }
  {
    ParsedCookie pc("httponly=foo");
    EXPECT_EQ(pc.ToCookieLine(), "httponly=foo");
  }
  {
    ParsedCookie pc("sameparty=foo");
    EXPECT_EQ(pc.ToCookieLine(), "sameparty=foo");
  }
  {
    ParsedCookie pc("foo");
    pc.SetName("secure");
    EXPECT_EQ(pc.ToCookieLine(), "secure=foo");
  }
  {
    ParsedCookie pc("bar");
    pc.SetName("httponly");
    EXPECT_EQ(pc.ToCookieLine(), "httponly=bar");
  }
  {
    ParsedCookie pc("foo=bar; baz=bob");
    EXPECT_EQ(pc.ToCookieLine(), "foo=bar; baz=bob");
  }
  // Outside of the first position, the value associated with a special name
  // should not be printed.
  {
    ParsedCookie pc("name=foo; secure");
    EXPECT_EQ(pc.ToCookieLine(), "name=foo; secure");
  }
  {
    ParsedCookie pc("name=foo; secure=bar");
    EXPECT_EQ(pc.ToCookieLine(), "name=foo; secure");
  }
  {
    ParsedCookie pc("name=foo; httponly=baz");
    EXPECT_EQ(pc.ToCookieLine(), "name=foo; httponly");
  }
  {
    ParsedCookie pc("name=foo; sameparty=baz");
    EXPECT_EQ(pc.ToCookieLine(), "name=foo; sameparty");
  }
  {
    ParsedCookie pc("name=foo; bar=secure");
    EXPECT_EQ(pc.ToCookieLine(), "name=foo; bar=secure");
  }
  // Repeated instances of the special tokens are also fine.
  {
    ParsedCookie pc("name=foo; secure; secure=yesplease; secure; secure");
    EXPECT_TRUE(pc.IsValid());
    EXPECT_TRUE(pc.IsSecure());
    EXPECT_FALSE(pc.IsHttpOnly());
    EXPECT_FALSE(pc.IsSameParty());
  }
  {
    ParsedCookie pc("sameparty; sameparty; secure; httponly; httponly; secure");
    EXPECT_EQ("", pc.Name());
    EXPECT_EQ("sameparty", pc.Value());
    EXPECT_TRUE(pc.IsSecure());
    EXPECT_TRUE(pc.IsSameParty());
    EXPECT_TRUE(pc.IsHttpOnly());
  }
  {
    ParsedCookie pc("partitioned=foo");
    EXPECT_EQ("partitioned", pc.Name());
    EXPECT_EQ("foo", pc.Value());
    EXPECT_FALSE(pc.IsPartitioned());
  }
  {
    ParsedCookie pc("partitioned=");
    EXPECT_EQ("partitioned", pc.Name());
    EXPECT_EQ("", pc.Value());
    EXPECT_FALSE(pc.IsPartitioned());
  }
  {
    ParsedCookie pc("=partitioned");
    EXPECT_EQ("", pc.Name());
    EXPECT_EQ("partitioned", pc.Value());
    EXPECT_FALSE(pc.IsPartitioned());
  }
  {
    ParsedCookie pc(
        "partitioned; partitioned; secure; httponly; httponly; secure");
    EXPECT_EQ("", pc.Name());
    EXPECT_EQ("partitioned", pc.Value());
    EXPECT_TRUE(pc.IsPartitioned());
  }
}

TEST(ParsedCookieTest, SameSiteValues) {
  struct TestCase {
    const char* cookie;
    bool valid;
    CookieSameSite mode;
  } cases[]{{"n=v; samesite=strict", true, CookieSameSite::STRICT_MODE},
            {"n=v; samesite=lax", true, CookieSameSite::LAX_MODE},
            {"n=v; samesite=none", true, CookieSameSite::NO_RESTRICTION},
            {"n=v; samesite=boo", true, CookieSameSite::UNSPECIFIED},
            {"n=v; samesite", true, CookieSameSite::UNSPECIFIED},
            {"n=v", true, CookieSameSite::UNSPECIFIED}};

  for (const auto& test : cases) {
    SCOPED_TRACE(test.cookie);
    ParsedCookie pc(test.cookie);
    EXPECT_EQ(test.valid, pc.IsValid());
    EXPECT_EQ(test.mode, pc.SameSite());
  }
}

TEST(ParsedCookieTest, InvalidNonAlphanumericChars) {
  ParsedCookie pc1("name=\x05");
  ParsedCookie pc2(
      "name=foo"
      "\x1c"
      "bar");
  ParsedCookie pc3(
      "name=foobar"
      "\x11");
  ParsedCookie pc4(
      "name=\x02"
      "foobar");

  ParsedCookie pc5("\x05=value");
  ParsedCookie pc6(
      "foo"
      "\x05"
      "bar=value");
  ParsedCookie pc7(
      "foobar"
      "\x05"
      "=value");
  ParsedCookie pc8(
      "\x05"
      "foobar"
      "=value");

  ParsedCookie pc9(
      "foo"
      "\x05"
      "bar"
      "=foo"
      "\x05"
      "bar");

  ParsedCookie pc10(
      "foo=bar;ba"
      "\x05"
      "z=boo");
  ParsedCookie pc11(
      "foo=bar;baz=bo"
      "\x05"
      "o");
  ParsedCookie pc12(
      "foo=bar;ba"
      "\05"
      "z=bo"
      "\x05"
      "o");

  ParsedCookie pc13(
      "foo=bar;ba"
      "\x7F"
      "z=bo");
  ParsedCookie pc14(
      "fo"
      "\x7F"
      "o=bar;"
      "z=bo");
  ParsedCookie pc15(
      "foo=bar"
      "\x7F"
      ";z=bo");

  EXPECT_FALSE(pc1.IsValid());
  EXPECT_FALSE(pc2.IsValid());
  EXPECT_FALSE(pc3.IsValid());
  EXPECT_FALSE(pc4.IsValid());
  EXPECT_FALSE(pc5.IsValid());
  EXPECT_FALSE(pc6.IsValid());
  EXPECT_FALSE(pc7.IsValid());
  EXPECT_FALSE(pc8.IsValid());
  EXPECT_FALSE(pc9.IsValid());
  EXPECT_FALSE(pc10.IsValid());
  EXPECT_FALSE(pc11.IsValid());
  EXPECT_FALSE(pc12.IsValid());
  EXPECT_FALSE(pc13.IsValid());
  EXPECT_FALSE(pc14.IsValid());
  EXPECT_FALSE(pc15.IsValid());
}

TEST(ParsedCookieTest, ValidNonAlphanumericChars) {
  // Note that some of these words are pasted backwords thanks to poor vim bidi
  // support. This should not affect the tests, however.
  const char pc1_literal[] = "name=العربية";
  const char pc2_literal[] = "name=普通話";
  const char pc3_literal[] = "name=ภาษาไทย";
  const char pc4_literal[] = "name=עִבְרִית";
  const char pc5_literal[] = "العربية=value";
  const char pc6_literal[] = "普通話=value";
  const char pc7_literal[] = "ภาษาไทย=value";
  const char pc8_literal[] = "עִבְרִית=value";
  ParsedCookie pc1(pc1_literal);
  ParsedCookie pc2(pc2_literal);
  ParsedCookie pc3(pc3_literal);
  ParsedCookie pc4(pc4_literal);
  ParsedCookie pc5(pc5_literal);
  ParsedCookie pc6(pc6_literal);
  ParsedCookie pc7(pc7_literal);
  ParsedCookie pc8(pc8_literal);

  EXPECT_TRUE(pc1.IsValid());
  EXPECT_EQ(pc1_literal, pc1.ToCookieLine());
  EXPECT_TRUE(pc2.IsValid());
  EXPECT_EQ(pc2_literal, pc2.ToCookieLine());
  EXPECT_TRUE(pc3.IsValid());
  EXPECT_EQ(pc3_literal, pc3.ToCookieLine());
  EXPECT_TRUE(pc4.IsValid());
  EXPECT_EQ(pc4_literal, pc4.ToCookieLine());
  EXPECT_TRUE(pc5.IsValid());
  EXPECT_EQ(pc5_literal, pc5.ToCookieLine());
  EXPECT_TRUE(pc6.IsValid());
  EXPECT_EQ(pc6_literal, pc6.ToCookieLine());
  EXPECT_TRUE(pc7.IsValid());
  EXPECT_EQ(pc7_literal, pc7.ToCookieLine());
  EXPECT_TRUE(pc8.IsValid());
  EXPECT_EQ(pc8_literal, pc8.ToCookieLine());
}

TEST(ParsedCookieTest, TruncatedNameOrValue) {
  using std::string_literals::operator""s;

  const char kCtlChars[] = {'\x0', '\xA', '\xD'};

  for (char ctl_char : kCtlChars) {
    std::string ctl_string(1, ctl_char);

    std::string truncated_name_string = "fo"s + ctl_string + "o=bar"s;
    ParsedCookie truncated_name(truncated_name_string);
    EXPECT_TRUE(truncated_name.IsValid());
    EXPECT_TRUE(truncated_name.HasTruncatedNameOrValue());

    std::string truncated_value_string = "foo=b"s + ctl_string + "ar"s;
    ParsedCookie truncated_value(truncated_value_string);
    EXPECT_TRUE(truncated_value.IsValid());
    EXPECT_TRUE(truncated_value.HasTruncatedNameOrValue());

    std::string not_truncated_string = "foo=bar"s + ctl_string;
    ParsedCookie not_truncated(not_truncated_string);
    EXPECT_TRUE(not_truncated.IsValid());
    EXPECT_FALSE(not_truncated.HasTruncatedNameOrValue());

    std::string not_truncated_string_extra_ctl_chars =
        "foo=bar"s + ctl_string + "\n\r\0"s;
    ParsedCookie not_truncated_extra_ctl_chars(
        not_truncated_string_extra_ctl_chars);
    EXPECT_TRUE(not_truncated_extra_ctl_chars.IsValid());
    EXPECT_FALSE(not_truncated_extra_ctl_chars.HasTruncatedNameOrValue());

    std::string not_truncated_string_whitespace =
        "foo=bar"s + ctl_string + " \t "s;
    ParsedCookie not_truncated_whitespace(not_truncated_string_whitespace);
    EXPECT_TRUE(not_truncated_whitespace.IsValid());
    EXPECT_FALSE(not_truncated_whitespace.HasTruncatedNameOrValue());

    std::string not_truncated_string_attribute_parsing =
        "foo=bar; Secure; Http"s + ctl_string + "Only"s;
    ParsedCookie not_truncated_attribute_parsing(
        not_truncated_string_attribute_parsing);
    EXPECT_TRUE(not_truncated_attribute_parsing.IsValid());
    EXPECT_TRUE(not_truncated_attribute_parsing.IsSecure());
    EXPECT_FALSE(not_truncated_attribute_parsing.IsHttpOnly());
    EXPECT_FALSE(not_truncated_attribute_parsing.HasTruncatedNameOrValue());
  }
}

}  // namespace net
