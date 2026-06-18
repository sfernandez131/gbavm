#---------------------------------------------------------------------------------------------------------------------
# gbavm - GBA Studio engine (Butano-based port of GB Studio's GBVM)
# Build config. The heavy lifting is done by butano.mak; this file just declares the project.
# See butano/examples/*/Makefile for the full list of documented options.
#---------------------------------------------------------------------------------------------------------------------
TARGET          :=  $(notdir $(CURDIR))
BUILD           :=  build
LIBBUTANO       :=  ../butano/butano
PYTHON          :=  python
SOURCES         :=  src
INCLUDES        :=  include
DATA            :=
GRAPHICS        :=  graphics
AUDIO           :=
AUDIOBACKEND    :=  null
AUDIOTOOL       :=
DMGAUDIO        :=
DMGAUDIOBACKEND :=  null
ROMTITLE        :=  GBAVM
ROMCODE         :=  GVME
USERFLAGS       :=
USERCXXFLAGS    :=
USERASFLAGS     :=
USERLDFLAGS     :=
USERLIBDIRS     :=
USERLIBS        :=
DEFAULTLIBS     :=
STACKTRACE      :=
USERBUILD       :=
EXTTOOL         :=

#---------------------------------------------------------------------------------------------------------------------
# Export absolute butano path:
#---------------------------------------------------------------------------------------------------------------------
ifndef LIBBUTANOABS
	export LIBBUTANOABS	:=	$(realpath $(LIBBUTANO))
endif

#---------------------------------------------------------------------------------------------------------------------
# Include main makefile:
#---------------------------------------------------------------------------------------------------------------------
include $(LIBBUTANOABS)/butano.mak
