# ESP32 VB6824 Audio Streaming

This project implements real-time audio streaming using an ESP32 microcontroller and VB6824 audio codec. It supports WebSocket communication for streaming Opus-encoded audio data.

## Features

- Real-time audio streaming using WebSocket protocol
- Opus codec integration for efficient audio compression
- VB6824 audio codec support for high-quality audio output
- WiFi connectivity for network communication
- Support for both text and binary WebSocket messages

## Hardware Requirements

- ESP32 development board
- VB6824 audio codec
- Speaker or audio output device
- Microphone (for audio input)

## Software Requirements

- ESP-IDF (Espressif IoT Development Framework)
- ESP-ADF (Espressif Audio Development Framework)
- Python 3.x (for server-side implementation)

## Configuration

1. Set up your WiFi credentials in `main/vb_demo.c`:
```c
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"
```

2. Configure WebSocket server address:
```c
#define WS_URI "ws://your_server_address:port"
```

## Building and Flashing

1. Set up ESP-IDF environment:
```bash
. $IDF_PATH/export.sh
```

2. Build the project:
```bash
idf.py build
```

3. Flash to ESP32:
```bash
idf.py -p (PORT) flash
```

4. Monitor the output:
```bash
idf.py -p (PORT) monitor
```

## Usage

1. Power on the ESP32 device
2. The device will automatically connect to the configured WiFi network
3. Once connected, it will establish a WebSocket connection to the server
4. Audio streaming will begin when the server sends Opus-encoded audio frames

## Project Structure

- `main/vb_demo.c`: Main application code
- `main/opus_codec.c`: Opus codec implementation
- `main/vb6824.c`: VB6824 audio codec driver

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request. 