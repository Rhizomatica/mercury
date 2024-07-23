/* -*- c++ -*- */
/* 
 * Copyright 2022 <+YOU OR YOUR COMPANY+>.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "gi_remover_impl.h"

namespace gr {
  namespace hermes_ofdm {

    gi_remover::sptr
    gi_remover::make(int Nfft, int Ngi)
    {
      return gnuradio::get_initial_sptr
        (new gi_remover_impl(Nfft, Ngi));
    }

    /*
     * The private constructor
     */
    gi_remover_impl::gi_remover_impl(int Nfft, int Ngi)
      : gr::block("gi_remover",
              gr::io_signature::make(1, 1, sizeof(gr_complex)*(Nfft+Ngi)),
              gr::io_signature::make(1, 1, sizeof(gr_complex)*Nfft))
    {
    	this->Nfft=Nfft;
    	this->Ngi=Ngi;
    }

    /*
     * Our virtual destructor.
     */
    gi_remover_impl::~gi_remover_impl()
    {
    }

    void
    gi_remover_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
    {
      ninput_items_required[0] = noutput_items;
    }

    int
    gi_remover_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      const gr_complex *in = (const gr_complex *) input_items[0];
      gr_complex *out = (gr_complex *) output_items[0];

      for(int i=0;i<noutput_items;i++)
      {
          for(int j=0;j<Nfft;j++)
          {
        	  out[i*Nfft+j]=in[i*(Nfft+Ngi)+j+Ngi];
          }

      }

      consume_each (noutput_items);

      return noutput_items;
    }

  } /* namespace hermes_ofdm */
} /* namespace gr */

