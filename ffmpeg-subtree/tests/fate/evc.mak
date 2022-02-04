FATE_EVC-$(call DEMDEC, EVC) += fate-evc

fate-evc: CODEC = libxeve
fate-evc: SRC = $(TARGET_SAMPLES)/evc/Test_Video_cif.yuv
fate-evc: FMT = evc
fate-evc: RAWDECOPTS = -r 30
fate-evc: CMD = enc_dec \
   "rawvideo -s 352x288 -pix_fmt yuv420p $(RAWDECOPTS)" \
   $(SRC) \
   $(FMT) \
   "-c:v $(CODEC) $(ENCOPTS)" \
   rawvideo \
   "-s 352x288 -pix_fmt yuv420p -vsync passthrough $(DECOPTS)" \
   "$(KEEP_OVERRIDE)" \
   "$(DECINOPTS)"

fate-evc: CMP = oneoff
fate-evc: CMP_UNIT = f32
fate-evc: FUZZ = 18


FATE_EVC-$(call DEMDEC, EVC) += fate-evc-mp4

fate-evc-mp4: CODEC = libxeve
fate-evc-mp4: SRC = $(TARGET_SAMPLES)/evc/Test_Video_cif.mp4
fate-evc-mp4: FMT = mp4
fate-evc-mp4: RAWDECOPTS = -r 30
fate-evc-mp4: CMD = transcode mp4 $(SRC) \
                          mp4 "-c:a copy -c:v libxeve" "-af aresample"

fate-evc-mp4: CMP = oneoff
fate-evc-mp4: CMP_UNIT = f32
fate-evc-mp4: FUZZ = 18

FATE_EVC += $(FATE_EVC-yes)
FATE_SAMPLES_FFMPEG += $(FATE_EVC)

