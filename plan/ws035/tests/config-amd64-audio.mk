# ws035-p006: the amd64 CI configuration with the audio framework selected
# although no audio hardware backend exists yet.
include config/ci/config-amd64.mk
KERN_AUDIO_BACKENDS := y
