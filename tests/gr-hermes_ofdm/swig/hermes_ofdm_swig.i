/* -*- c++ -*- */

#define HERMES_OFDM_API

%include "gnuradio.i"			// the common stuff

//load generated python docstrings
%include "hermes_ofdm_swig_doc.i"

%{
#include "hermes_ofdm/zero_padder.h"
#include "hermes_ofdm/zero_depadder.h"
#include "hermes_ofdm/gi_adder.h"
#include "hermes_ofdm/gi_remover.h"
#include "hermes_ofdm/framer.h"
#include "hermes_ofdm/deframer.h"
%}


%include "hermes_ofdm/zero_padder.h"
GR_SWIG_BLOCK_MAGIC2(hermes_ofdm, zero_padder);
%include "hermes_ofdm/zero_depadder.h"
GR_SWIG_BLOCK_MAGIC2(hermes_ofdm, zero_depadder);
%include "hermes_ofdm/gi_adder.h"
GR_SWIG_BLOCK_MAGIC2(hermes_ofdm, gi_adder);
%include "hermes_ofdm/gi_remover.h"
GR_SWIG_BLOCK_MAGIC2(hermes_ofdm, gi_remover);
%include "hermes_ofdm/framer.h"
GR_SWIG_BLOCK_MAGIC2(hermes_ofdm, framer);
%include "hermes_ofdm/deframer.h"
GR_SWIG_BLOCK_MAGIC2(hermes_ofdm, deframer);
