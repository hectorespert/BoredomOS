#include <MAVLinkFTP.h>
#include <Arduino.h>

MAVLinkFTP::MAVLinkFTP()
{

}
    
struct FtpPayload
{
    uint16_t seq_number;
    uint8_t session;
    uint8_t opcode;
    uint8_t size;
    uint8_t req_opcode;
    uint8_t	burst_complete;
	uint8_t	padding;
	uint32_t offset;
	uint8_t	data[];		
};

void MAVLinkFTP::handle(mavlink_file_transfer_protocol_t *ftp_request, uint8_t system_id, uint8_t component_id)
{
    if ((ftp_request->target_system != system_id && ftp_request->target_system != 0) ||
        (ftp_request->target_component != component_id && ftp_request->target_component != 0)) {
		return;
    }

    FtpPayload *ftpPayload = reinterpret_cast<FtpPayload*>(ftp_request->payload);
    
    /*Serial.print("FTP Sequence Number: ");
    Serial.println(ftpPayload->seq_number);
    Serial.print("FTP Session: ");
    Serial.println(ftpPayload->session);
    Serial.print("FTP Opcode: ");
    Serial.println(ftpPayload->opcode);
    Serial.print("FTP Size: ");
    Serial.println(ftpPayload->size);
    Serial.print("FTP Request Opcode: ");
    Serial.println(ftpPayload->req_opcode);
    Serial.print("FTP Burst Complete: ");
    Serial.println(ftpPayload->burst_complete);
    Serial.print("FTP Offset: ");
    Serial.println(ftpPayload->offset);
    Serial.print("FTP Data: ");
    for (uint8_t i = 0; i < ftpPayload->size; ++i) {
        Serial.print(ftpPayload->data[i], HEX);
        Serial.print(" ");
    }
    Serial.println();*/

    ftp_request->target_system = system_id;
    ftp_request->target_component = component_id;
    ftp_request->target_network = 0;
    
    ftpPayload->seq_number++;
    ftpPayload->req_opcode = ftpPayload->opcode;
    ftpPayload->opcode = 129;
    ftpPayload->size = 1;
    ftpPayload->data[0] = 1;
}