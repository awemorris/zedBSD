# ws035-p006: the pcat CI configuration with the audio framework selected
# although no audio hardware backend exists yet.
include config/ci/config-pcat.mk
KERN_AUDIO_BACKENDS := y
