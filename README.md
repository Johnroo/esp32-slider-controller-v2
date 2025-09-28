# ESP32 Slider Controller V2 🎬

**Professional 4-axis camera slider system with advanced motion control**

[![ESP32](https://img.shields.io/badge/ESP32-Dev%20Module-blue)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-IDE-green)](https://platformio.org/)
[![Python](https://img.shields.io/badge/Python-3.8+-yellow)](https://python.org/)
[![License](https://img.shields.io/badge/License-MIT-red)](LICENSE)

## 🚀 Features

### 🎮 Advanced Motion Control
- **4-axis control**: Pan, Tilt, Zoom, Slide with TMC2209 drivers
- **Synchronized presets**: Simultaneous finish on all axes
- **Joystick pipeline**: Deadzone, expo, filtering, slew limiting
- **Slide-to-pan/tilt coupling**: Automatic subject centering
- **Minimum-jerk profiles**: Cinema-quality smooth moves

### 🎯 Joystick Control
- **Real-time Pan/Tilt jog**: Immediate response when idle
- **Configurable parameters**: Deadzone (0.06), expo (0.35), slew (8000)
- **IIR filtering**: 60Hz for smooth control
- **Combined control**: `/joy/pt` route for 2D joystick

### 📡 OSC Communication
- **Robust protocol**: Proper padding for all messages
- **Routes**: `/pan`, `/tilt`, `/slide/jog`, `/slide/goto`, `/preset/set`, `/preset/recall`
- **Configuration**: `/config/offset_range`, `/config/pan_map`, `/config/tilt_map`
- **Joystick config**: `/joy/config` for runtime adjustment

### 🌐 Web Interface
- **Advanced HTML interface**: Real-time control at `/advanced`
- **Joystick configuration**: Live parameter adjustment
- **Preset management**: Duration control and recall
- **Slide controls**: Jog and goto with timing
- **Configuration panel**: Ranges and mappings

## 🔧 Technical Specifications

### Hardware
- **ESP32 Dev Module**: 240MHz, 320KB RAM, 4MB Flash
- **4x TMC2209**: Stepper drivers with UART communication
- **FastAccelStepper**: High-performance motion control
- **WiFi connectivity**: OTA updates and web interface

### Software Stack
- **PlatformIO**: Cross-platform development
- **Arduino Framework**: ESP32 support
- **FastAccelStepper**: Motion control library
- **TMCStepper**: Driver communication
- **CNMAT OSC**: Open Sound Control protocol
- **WiFiManager**: Easy network setup
- **ESPAsyncWebServer**: Web interface
- **Flask**: Python web controller

## 📋 Quick Start

### 1. Hardware Setup
```
ESP32 Pinout:
- Pan:   STEP=18, DIR=19, EN=13
- Tilt:  STEP=21, DIR=22, EN=14  
- Zoom:  STEP=23, DIR=25, EN=32
- Slide: STEP=26, DIR=27, EN=33
- UART:  TX=17, RX=16 (TMC2209)
```

### 2. Software Installation
```bash
# Clone repository
git clone https://github.com/johnroo/esp32-slider-controller-v2.git
cd esp32-slider-controller-v2

# Install Python dependencies
pip install -r requirements.txt

# Upload firmware (PlatformIO)
pio run --target upload

# Start web controller
python3 start_controller.py
```

### 3. Configuration
1. **Connect ESP32** via USB
2. **Upload firmware** with PlatformIO
3. **Connect to WiFi** (ESP32 creates AP "SliderAP")
4. **Open web interface**: `http://localhost:9000`
5. **Advanced interface**: `http://localhost:9000/advanced`

## 🎬 Usage

### Basic Control
- **Web interface**: Direct axis control with sliders
- **Joystick**: Real-time Pan/Tilt with smooth response
- **Presets**: Save and recall positions with timing
- **Slide jog**: Continuous movement control

### Advanced Features
- **Synchronized moves**: All axes finish simultaneously
- **Subject centering**: Slide movement automatically adjusts Pan/Tilt
- **Joystick tuning**: Adjust deadzone, expo, filtering in real-time
- **OSC control**: External software integration

## 📡 OSC Protocol

### Motion Control
```
/pan <float>           # Pan joystick (-1.0 to 1.0)
/tilt <float>          # Tilt joystick (-1.0 to 1.0)
/joy/pt <float> <float> # Combined pan/tilt
/slide/jog <float>     # Slide jog (-1.0 to 1.0)
/slide/goto <float> <float> # Slide goto (0.0-1.0, duration)
```

### Presets
```
/preset/set <int> <int> <int> <int> <int> # Set preset (id, pan, tilt, zoom, slide)
/preset/recall <int> <float>              # Recall preset (id, duration)
```

### Configuration
```
/config/offset_range <int> <int>          # Pan/tilt offset ranges
/config/pan_map <int> <int>               # Slide→pan mapping
/config/tilt_map <int> <int>              # Slide→tilt mapping
/joy/config <float> <float> <float> <float> # Joystick params (deadzone, expo, slew, filter)
```

## 🛠️ Development

### Project Structure
```
esp32-slider-controller-v2/
├── src/main.cpp              # ESP32 firmware
├── slider_controller.py      # Flask web controller
├── templates/
│   ├── index.html           # Basic interface
│   └── advanced.html        # Advanced interface
├── platformio.ini          # PlatformIO configuration
├── requirements.txt         # Python dependencies
└── README.md               # This file
```

### Building
```bash
# Compile firmware
pio run

# Upload to ESP32
pio run --target upload

# Monitor serial output
pio device monitor
```

### Customization
- **Motor parameters**: Edit `cfg[]` array in `main.cpp`
- **Pin assignments**: Modify `STEP_PINS[]`, `DIR_PINS[]`, `ENABLE_PINS[]`
- **OSC routes**: Add new handlers in `processOSC()`
- **Web interface**: Customize HTML templates

## 📊 Performance

### Motion Quality
- **Smooth acceleration**: FastAccelStepper profiles
- **No jitter**: TMC2209 microstepping
- **Precise timing**: Synchronized movements
- **Cinema quality**: Minimum-jerk profiles

### Response Time
- **Joystick**: < 10ms response
- **OSC**: < 5ms processing
- **Web interface**: Real-time updates
- **Preset recall**: Configurable timing

## 🤝 Contributing

1. Fork the repository
2. Create feature branch (`git checkout -b feature/amazing-feature`)
3. Commit changes (`git commit -m 'Add amazing feature'`)
4. Push to branch (`git push origin feature/amazing-feature`)
5. Open Pull Request

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- **FastAccelStepper**: High-performance stepper control
- **TMCStepper**: TMC2209 driver communication
- **CNMAT OSC**: Open Sound Control protocol
- **ESPAsyncWebServer**: Web interface framework
- **PlatformIO**: Cross-platform development

## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/johnroo/esp32-slider-controller-v2/issues)
- **Discussions**: [GitHub Discussions](https://github.com/johnroo/esp32-slider-controller-v2/discussions)
- **Documentation**: [Wiki](https://github.com/johnroo/esp32-slider-controller-v2/wiki)

---

**Ready for professional cinematography! 🎥✨**

*Built with ❤️ for the filmmaking community*