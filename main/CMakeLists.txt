############### Add include ###################
list(APPEND ADD_INCLUDE "include")
list(APPEND ADD_PRIVATE_INCLUDE "src")
###############################################

############ Add source files #################
append_srcs_dir(ADD_SRCS "src")
###############################################

###### Add required/dependent components ######
# MaixCAM2's nn runtime ships libms_asr_ax630c.so, which itself needs ALSA.
# Declare alsa_lib explicitly because the current nn component metadata does
# not propagate it on maixcam2; otherwise a clean board can fail at startup.
list(APPEND ADD_REQUIREMENTS basic nn vision alsa_lib)
###############################################

register_component()
