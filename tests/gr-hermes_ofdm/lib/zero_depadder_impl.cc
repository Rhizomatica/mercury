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
#include "zero_depadder_impl.h"

namespace gr {
  namespace hermes_ofdm {

    zero_depadder::sptr
    zero_depadder::make(int Nc, int Nfft)
    {
      return gnuradio::get_initial_sptr
        (new zero_depadder_impl(Nc, Nfft));
    }

    /*
     * The private constructor
     */
    zero_depadder_impl::zero_depadder_impl(int Nc, int Nfft)
      : gr::block("zero_depadder",
              gr::io_signature::make(1, 1, sizeof(gr_complex)*Nfft),
              gr::io_signature::make(1, 1, sizeof(gr_complex)*Nc))
    {
    	this->Nc=Nc;
    	this->Nfft=Nfft;
	}

    /*
     * Our virtual destructor.
     */
    zero_depadder_impl::~zero_depadder_impl()
    {
    }

    void
    zero_depadder_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
    {
      ninput_items_required[0] = noutput_items;
    }

    int
    zero_depadder_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      const gr_complex *in = (const gr_complex *) input_items[0];
      gr_complex *out = (gr_complex *) output_items[0];

      for(int i=0;i<noutput_items;i++)
      {
          for(int j=0;j<Nc/2;j++)
          {
        	  out[i*Nc+j]=in[i*Nfft+j+Nfft-Nc/2];
          }
          for(int j=Nc/2;j<Nc;j++)
          {
        	  out[i*Nc+j]=in[i*Nfft+j-Nc/2];
          }
      }

      consume_each (noutput_items);

      return noutput_items;
    }

  } /* namespace hermes_ofdm */
} /* namespace gr */

