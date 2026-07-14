# AMY → WerewolfAmyS3

This AMY checkout is the synth engine for the WerewolfAmyS3 firmware
(ESP32-S3 port of the werewolf sequencer & synth):

  https://github.com/Leeman1982/werewolf — branch
  `claude/werewolf-sequencer-esp32-busn28`, directory `WerewolfAmyS3/`

AMY is used **unmodified** as an Arduino library (see that repo's
`platformio.ini` / README for how it is pulled in). The firmware builds a
custom 3-oscillator voice as an AMY memory patch (`wolf_synth.h`), uses the
Juno-106/DX7 factory banks, and drives its sequencers from AMY's 48 PPQ
sequencer hook with external MIDI clock sync support.
