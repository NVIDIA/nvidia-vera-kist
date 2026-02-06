#include <cerrno>
#include <cstdint>
#include <string>

int change_integer_value(const int& requested_value, int& current_value)
{
    if (requested_value >= 50)
    {
        return -EINVAL;
    }
    current_value = requested_value;
    return 1;
}

std::string unlock_door(const std::string& key_val)
{
    if (key_val != "open sesame")
    {
        return "DoorLocked";
    }
    return "DoorUnlocked";
}