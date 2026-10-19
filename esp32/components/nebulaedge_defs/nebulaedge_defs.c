#include "nebulaedge_defs.h"
#include "schema.pb-c.h"     // PacketType

const uint8_t DEEP_SLEEP_FLAG[DEEP_SLEEP_FLAG_LEN] = {PACKET_TYPE__PACKET_TYPE_DEEP_SLEEP, 'd', 's'};
