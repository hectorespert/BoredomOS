#ifndef MAVLINK_FTP_H
#define MAVLINK_FTP_H

#include <cstdint>
#include <MAVLink.h>

class MAVLinkFTP    
{
public:
  MAVLinkFTP();
  void handle(mavlink_file_transfer_protocol_t *ftp_request, uint8_t system_id, uint8_t component_id);
private:
};

#endif