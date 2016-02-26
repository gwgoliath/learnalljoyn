#!/bin/bash
scons BINDINGS=cpp WS=off BT=off ICE=off CRYPTO=builtin CPU=arm CROSS_COMPILE=/opt/gcc-linaro-arm-linux-gnueabihf-raspbian/bin/arm-linux-gnueabihf- SERVICES="about,notification,controlpanel,config,onboarding,sample_apps"
