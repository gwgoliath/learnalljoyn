#!/bin/bash
scons BINDINGS=cpp WS=off BT=off ICE=off CRYPTO=builtin  SERVICES="about,notification,controlpanel,config,onboarding,sample_apps"
