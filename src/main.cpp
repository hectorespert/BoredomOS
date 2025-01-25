#include <Arduino.h>
#include <millisDelay.h>
#include <MAVLink.h>

#define LED_HIGH_MS 250
// #define LED_LOW_MS 59750
#define LED_LOW_MS 750

#define MAVLINK_HEARTBEAT 1000

static millisDelay ledDelay;
static millisDelay mavlinkHeartbeatDelay;

void setup()
{
  Serial.begin(9600);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  ledDelay.start(LED_LOW_MS);

  mavlinkHeartbeatDelay.start(MAVLINK_HEARTBEAT);
}

void changeStatusLed()
{
  if (ledDelay.justFinished())
  {
    bool ledStatus = digitalRead(LED_BUILTIN);
    if (ledStatus == LOW)
    {
      ledDelay.start(LED_HIGH_MS);
    }
    else
    {
      ledDelay.start(LED_LOW_MS);
    }
    digitalWrite(LED_BUILTIN, !ledStatus);
  }
}

void sendHeartbeat()
{
  if (mavlinkHeartbeatDelay.justFinished())
  {
    mavlinkHeartbeatDelay.repeat();

    mavlink_message_t msg;
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];

    mavlink_msg_heartbeat_pack(1, MAV_COMP_ID_AUTOPILOT1, &msg, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_GENERIC, MAV_MODE_FLAG_MANUAL_INPUT_ENABLED, 0, MAV_STATE_STANDBY);
    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);

    Serial.write(buf, len);
  }
}

void loop()
{
  changeStatusLed();

  sendHeartbeat();
}
