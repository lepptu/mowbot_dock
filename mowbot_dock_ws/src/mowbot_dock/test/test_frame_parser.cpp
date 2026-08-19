#include <gtest/gtest.h>

#include "frame_parser.hpp"

using mowbot_dock::parse_status_frame;
using mowbot_dock::is_event_line;

TEST(ParseStatusFrame, SpecExample)
{
  const auto frame = parse_status_frame("3,1,1,1,1.87,41.8,0,3721");
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->state, 3);
  EXPECT_TRUE(frame->microswitch);
  EXPECT_TRUE(frame->k1);
  EXPECT_TRUE(frame->k2);
  EXPECT_FLOAT_EQ(frame->charge_current, 1.87f);
  EXPECT_FLOAT_EQ(frame->charger_voltage, 41.8f);
  EXPECT_EQ(frame->fault_code, 0);
  EXPECT_EQ(frame->uptime_s, 3721u);
}

TEST(ParseStatusFrame, ColdIdle)
{
  const auto frame = parse_status_frame("0,0,0,0,0.00,0.1,0,5");
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->state, 0);
  EXPECT_FALSE(frame->microswitch);
  EXPECT_FALSE(frame->k1);
  EXPECT_FALSE(frame->k2);
}

TEST(ParseStatusFrame, ToleratesWhitespaceAndCarriageReturn)
{
  const auto frame = parse_status_frame(" 7 , 1 ,0, 0 ,0.0,0.0, 2 ,99\r");
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->state, 7);
  EXPECT_EQ(frame->fault_code, 2);
}

TEST(ParseStatusFrame, NegativeCurrentAllowed)
{
  // Sensor noise around zero can read slightly negative.
  const auto frame = parse_status_frame("4,1,1,0,-0.02,38.5,0,100");
  ASSERT_TRUE(frame.has_value());
  EXPECT_FLOAT_EQ(frame->charge_current, -0.02f);
}

TEST(ParseStatusFrame, FutureFaultCodePassesThrough)
{
  const auto frame = parse_status_frame("7,1,0,0,0.0,0.0,9,100");
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->fault_code, 9);
}

TEST(ParseStatusFrame, RejectsWrongFieldCount)
{
  EXPECT_FALSE(parse_status_frame("").has_value());
  EXPECT_FALSE(parse_status_frame("3,1,1,1,1.87,41.8,0").has_value());
  EXPECT_FALSE(parse_status_frame("3,1,1,1,1.87,41.8,0,3721,9").has_value());
}

TEST(ParseStatusFrame, RejectsNonNumeric)
{
  EXPECT_FALSE(parse_status_frame("x,1,1,1,1.87,41.8,0,3721").has_value());
  EXPECT_FALSE(parse_status_frame("3,1,1,1,1.87v,41.8,0,3721").has_value());
  EXPECT_FALSE(parse_status_frame("3,1,1,1,,41.8,0,3721").has_value());
}

TEST(ParseStatusFrame, RejectsOutOfRangeEnums)
{
  EXPECT_FALSE(parse_status_frame("8,1,1,1,1.87,41.8,0,3721").has_value());
  EXPECT_FALSE(parse_status_frame("-1,1,1,1,1.87,41.8,0,3721").has_value());
  EXPECT_FALSE(parse_status_frame("3,2,1,1,1.87,41.8,0,3721").has_value());
  EXPECT_FALSE(parse_status_frame("3,1,1,1,1.87,41.8,-1,3721").has_value());
  EXPECT_FALSE(parse_status_frame("3,1,1,1,1.87,41.8,0,-1").has_value());
}

TEST(ParseStatusFrame, RejectsEventAndGarbageLines)
{
  EXPECT_FALSE(parse_status_frame("EVT:BOOT:1.0").has_value());
  EXPECT_FALSE(parse_status_frame("!#garbage@@").has_value());
}

TEST(IsEventLine, Classification)
{
  EXPECT_TRUE(is_event_line("EVT:BOOT:1.0"));
  EXPECT_TRUE(is_event_line("EVT:SELFTEST:OK"));
  EXPECT_TRUE(is_event_line("EVT:NOCURRENT"));
  EXPECT_FALSE(is_event_line("3,1,1,1,1.87,41.8,0,3721"));
  EXPECT_FALSE(is_event_line(" EVT:BOOT"));
  EXPECT_FALSE(is_event_line(""));
}

TEST(Names, StateAndFaultNames)
{
  EXPECT_STREQ(mowbot_dock::state_name(0), "IDLE");
  EXPECT_STREQ(mowbot_dock::state_name(7), "FAULT");
  EXPECT_STREQ(mowbot_dock::state_name(8), "?");
  EXPECT_STREQ(mowbot_dock::fault_name(0), "none");
  EXPECT_STREQ(mowbot_dock::fault_name(4), "watchdog silence");
  EXPECT_STREQ(mowbot_dock::fault_name(9), "?");
}
