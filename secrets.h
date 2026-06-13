#ifndef SECRETS_H
#define SECRETS_H

// ============================================================
//  secrets.h - Private configuration for eINK weather station
// ============================================================
//
//  Replace the placeholder values below with your own credentials.
//  This file is intentionally committed as a template — the real
//  values are kept safe in your local working copy.
//
//  WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// LoRaWAN OTAA credentials (obtain from your LoRaWAN network server,
// e.g. The Things Stack, ChirpStack, etc.)
uint8_t devEui[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
uint8_t appEui[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
uint8_t appKey[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

#endif // SECRETS_H
