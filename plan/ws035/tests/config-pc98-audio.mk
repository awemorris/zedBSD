# ws035-p006: the pc98 CI configuration with the audio framework selected
# although no audio hardware backend exists yet.
include config/ci/config-pc98.mk
KERN_AUDIO_BACKENDS := y
