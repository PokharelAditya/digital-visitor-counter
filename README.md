# ESP32 Digital Visitor Counter

A real-time bidirectional visitor counting system using ESP32 and dual IR sensors to track room entry, exit, and occupancy with WiFi dashboard and cloud integration.

## Features

### Core Functionality
- **Bidirectional Counting**: Tracks both entries and exits using a finite state machine
- **Real-time Occupancy**: Calculates current room occupancy (entries - exits)
- **Capacity Management**: Configurable maximum capacity with LED alert when full
- **Anti-bounce Protection**: Prevents negative occupancy counts
- **Persistent Storage**: Counters survive power cycles using NVS (Non-Volatile Storage)

### Data Logging & Visualization
- **Local Event Logging**: CSV format logs stored on SPIFFS with automatic rotation
- **WiFi Dashboard**: Embedded web interface for real-time monitoring
- **Export Capability**: Download historical logs as CSV files

### Cloud Integration
- **Firebase Realtime Database**: Real-time event streaming and status updates
- **ThingSpeak**: Periodic data uploads for analytics and visualization

### User Interfaces
- **Web Dashboard**: Dark-themed responsive UI with auto-refresh
- **Serial Console**: UART command interface for debugging and manual control
- **Manual Reset**: Physical button for counter reset

## Hardware Components

### Required Components
- **ESP32 Development Board** (any variant with WiFi)
- **2x IR Obstacle Avoidance Sensors** (active-LOW output)
- **1x LED** (for capacity alert indication)
- **1x Push Button** (for manual reset)
- **Resistors** (as needed for LED current limiting)
- **Power Supply** (5V via USB or external)

### Pin Configuration
Configure the following pins in `config.h`:
- `IR1_PIN` - Entry sensor (active-LOW)
- `IR2_PIN` - Exit sensor (active-LOW)
- `LED_PIN` - Room full indicator
- `RESET_BTN_PIN` - Manual reset button (active-LOW with internal pull-up)

## Software Architecture

### Multi-tasking Design
Built on ESP-IDF and FreeRTOS with three concurrent tasks:
- **sensor_task** (Core 1, High Priority) - Fast FSM for sensor processing
- **cloud_task** (Core 0, Medium Priority) - Non-blocking HTTP uploads via queue
- **serial_console_task** (Core 0, Medium Priority) - UART command handler

### Finite State Machine
Three-state FSM prevents false counts:
1. **IDLE** - Waiting for sensor trigger
2. **IR1_TRIGGERED** - Waiting for IR2 to confirm entry
3. **IR2_TRIGGERED** - Waiting for IR1 to confirm exit

Features configurable debounce delay and event timeout for reliability.

## Getting Started

### Prerequisites
- [ESP-IDF v4.4+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) installed and configured
- Python 3.7+ with ESP-IDF tools
- USB cable for flashing and serial monitoring

### Installation

1. **Clone the repository**
   ```bash
   git clone <repository-url>
   cd Visitor-Counter-ESP32
   ```

2. **Configure the project**
   ```bash
   cp config.example.h config.h
   ```

3. **Edit `config.h` with your settings**
   ```c
   // WiFi Credentials
   #define WIFI_SSID "YourWiFiName"
   #define WIFI_PASS "YourPassword"
   
   // Hardware Pin Definitions
   #define IR1_PIN 18
   #define IR2_PIN 19
   #define LED_PIN 2
   #define RESET_BTN_PIN 0
   
   // Timing Configuration
   #define DEBOUNCE_MS 50
   #define EVENT_TIMEOUT_MS 5000
   
   // Capacity Settings
   #define DEFAULT_MAX_CAPACITY 50
   
   // Feature Toggles
   #define ENABLE_LOCAL_LOG 1
   #define ENABLE_WIFI_DASHBOARD 1
   #define ENABLE_FIREBASE 1
   #define ENABLE_THINGSPEAK 1
   
   // Cloud Service URLs (if enabled)
   #define FIREBASE_URL "https://your-project.firebaseio.com"
   #define FIREBASE_SECRET "your-database-secret"
   #define THINGSPEAK_API_KEY "your-api-key"
   ```

4. **Build and flash**
   ```bash
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

### Usage

#### Web Dashboard
1. Connect to the same WiFi network as the ESP32
2. Find the ESP32 IP address in serial monitor output
3. Open `http://<ESP32-IP>/` in your browser
4. Monitor real-time counts, occupancy, and recent events

#### Serial Console Commands
Connect via UART at 115200 baud:
- `STATUS` - Display current counters
- `RESET` - Reset all counters to zero
- `SETCAP <n>` - Set maximum capacity
- `CLEARLOG` - Clear event log
- `HELP` - Show available commands

#### Web API Endpoints
- `GET /` - Dashboard HTML
- `GET /status` - JSON status (entries, exits, occupancy)
- `GET /log` - Recent events as JSON
- `GET /export` - Download CSV log file
- `GET /reset` - Reset all counters
- `GET /clearlog` - Clear event log

## Project Structure

```
Visitor-Counter-ESP32/
├── main/
│   ├── main.c              # Core FSM logic and task coordination
│   ├── data_store.c/h      # NVS persistence and SPIFFS logging
│   ├── wifi_manager.c/h    # WiFi connection and NTP time sync
│   ├── web_server.c/h      # HTTP server and cloud integrations
│   └── CMakeLists.txt
├── config.h                # User configuration (gitignored)
├── config.example.h        # Configuration template
├── dashboard_preview.html  # Standalone dashboard preview
├── partitions.csv          # Custom partition table
├── CMakeLists.txt          # Main build configuration
└── README.md
```

## Configuration Options

### Storage Settings
- `LOG_FILE_PATH` - Path to CSV log file on SPIFFS
- `LOG_MAX_BYTES` - Maximum log size before rotation
- `LOG_JSON_MAX_ROWS` - Maximum events returned via `/log` API
- `SAVE_EVERY_N_EVENTS` - Auto-save frequency to NVS

### Network Settings
- `NTP_SERVER` - NTP server for time synchronization (default: pool.ntp.org)
- `THINGSPEAK_INTERVAL_MS` - Upload interval for ThingSpeak

### Firebase Configuration
Requires:
- `FIREBASE_URL` - Your Firebase Realtime Database URL
- `FIREBASE_SECRET` - Database authentication secret

### ThingSpeak Configuration
Requires:
- `THINGSPEAK_API_KEY` - Write API key for your channel
- Maps to fields: Field1=Entries, Field2=Exits, Field3=Occupancy

## Partition Table

Custom partition layout (4MB flash):
- **NVS** (24KB) - Counter persistence
- **Factory** (1.5MB) - Application firmware
- **SPIFFS** (2MB) - Event log storage

## Troubleshooting

### Compilation Errors
- Ensure all constants in `config.h` are properly defined
- Check ESP-IDF version compatibility (v4.4+)
- Verify all components are enabled in `sdkconfig`

### WiFi Connection Issues
- Verify SSID and password in `config.h`
- Check WiFi signal strength
- Monitor serial output for connection status

### Sensor Not Detecting
- Verify sensor wiring and power supply
- Check sensor LED indicators
- Adjust sensor sensitivity potentiometer
- Confirm correct pin definitions in `config.h`

### False Counts
- Increase `DEBOUNCE_MS` value
- Adjust `EVENT_TIMEOUT_MS` for slower passages
- Ensure proper sensor placement and alignment

### Cloud Upload Failures
- Verify internet connectivity
- Check API keys and URLs in `config.h`
- Monitor serial output for HTTP error codes
- For Firebase: ensure database rules allow writes

## Technical Specifications

- **Platform**: ESP-IDF (FreeRTOS)
- **Language**: C
- **Flash**: 4MB (recommended)
- **RAM**: ~100KB runtime usage
- **WiFi**: 802.11 b/g/n (2.4GHz)
- **Time Sync**: SNTP/NTP with timezone support
- **Security**: TLS 1.2 for Firebase HTTPS

## Design Considerations

- **Interrupt-driven sensors**: ISR flags with polled reading to prevent race conditions
- **Core separation**: Time-critical FSM on Core 1, slow I/O on Core 0
- **Non-blocking cloud**: Queue-based design prevents HTTP timeouts from blocking sensors
- **Automatic log rotation**: Maintains latest events when storage limit reached

## Future Enhancements

Potential improvements:
- [ ] MQTT support for home automation integration
- [ ] Mobile app for remote monitoring
- [ ] Multiple entrance support
- [ ] Historical analytics and trends
- [ ] OLED/LCD display for local readout
- [ ] Buzzer alerts for capacity warnings

## License

This project is open source. Please check the repository for license details.

## Contributing

Contributions are welcome! Please feel free to submit issues, fork the repository, and create pull requests.

## Acknowledgments

Migrated from Arduino to ESP-IDF for improved performance and professional architecture. Built with ESP-IDF framework and FreeRTOS.

---

**Note**: This is a production-quality firmware designed for reliability and scalability. The finite state machine approach ensures accurate counting even in high-traffic scenarios.
