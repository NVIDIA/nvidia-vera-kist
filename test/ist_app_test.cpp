#include <ist_app.hpp>

#include <gtest/gtest.h>

TEST(TemplateApp, changeIntegerValue)
{
    int current_value = 0;
    // Good path.  Expect that changing the value to 42 should change it
    EXPECT_EQ(1, change_integer_value(42, current_value));
    EXPECT_EQ(current_value, 42);

    // Bad path, expecting that changing to 51 should return an error, and keep
    // the current value
    EXPECT_EQ(-EINVAL, change_integer_value(51, current_value));
    EXPECT_EQ(current_value, 42);
}

TEST(TemplateApp, unlockDoor)
{
    // Good path.  Expect that changing the value to 42 should change it
    EXPECT_EQ("DoorLocked", unlock_door(""));
    EXPECT_EQ("DoorLocked", unlock_door("BadKeyValue"));
    EXPECT_EQ("DoorUnlocked", unlock_door("open sesame"));
}