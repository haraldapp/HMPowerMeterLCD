# HMPowerMeterLCD
 Smarthome Energie Zähler mit Impulseingängen
 Status: Prototyp im Dauertest

 ![](http://http://https://github.com/haraldapp/HMPowerMeterLCD/wiki/project.jpg)
 ![](http://http://https://github.com/haraldapp/HMPowerMeterLCD/wiki/counter.jpg)

##  Features
- LCD Display mit Menüsteuerung
- 2 getrennte Impluseingänge für 2 Zähler
- LiFePo4 Akku zum Betrieb oder zur Überbrückung von Spannungsausfällen
- Eingabemöglichkeit des aktuellen Zählerstands, um den Wert vom Zähler am Display und an der Zentrale anzuzeigen

## Hardware
- Arduino Pro Mini 3,3V
- LCD Display 84x48 (z.B. Nokia 5110)
- CC1101 Modul
- LiFePo4 AA Akku mit Batteriehalter
- TP5000 Lademodul
- 3DPrint Gehäuse zum Prototypenaufbau mit Verdrahtung direkt im Gehäuse

## Software
- [ASKSINPP](https://github.com/pa-pa/AskSinPP) basierender ARDUINO Sketch auf Basis [HM-ES-TX-WM]( https://github.com/jp112sdl/Beispiel_AskSinPP/tree/master/examples/HM-ES-TX-WM_CCU)
- Umfangreiche Displayanzeige von LED Status, Eingang Status, Batterie und Zählerständen
- Menüsteuerung zur Eingabe des aktuellen Zählerstand
- Permanente Speicherung des Zählerstand mit Backup im EEPROM

## Getting Started
- Schaltplan [```PDF```](http://https://github.com/haraldapp/HMPowerMeterLCD/schematic/HMPowerMeterSchematic.pdf) [```JPG```](http://https://github.com/haraldapp/HMPowerMeterLCD/schematic/HMPowerMeterSchematic.jpg)
- Verdrahtung (Beispiel) [```JPG```](http://https://github.com/haraldapp/HMPowerMeterLCD/schematic/HMPowerMeterWire.jpg)
- Gehäuse [```JPG```](http://https://github.com/haraldapp/HMPowerMeterLCD/case/HMPowerMeterCase.jpg) (STL Dateien im Verzeichnis *case*)
- Sketch [```ino```](http://https://github.com/haraldapp/HMPowerMeterLCD/HMPowerMeterLCD.ino)
- Details ...folgen noch...

## Lizenz
- [Creative Commons 3.0](http://creativecommons.org/licenses/by-nc-sa/3.0/de/)

## 
**Thanks to**
> [pa-pa](https://github.com/pa-pa) für seine umfangreiche Arbeit
> [Jérôme](https://github.com/jp112sdl) für seine umfangreiche Arbeit
> [JurgenO](https://github.com/JurgenO) for seine Ideen und Arbeit -> [GasMetering](https://github.com/JurgenO/GasMetering)


