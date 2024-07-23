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
#include "gi_adder_impl.h"

namespace gr {
  namespace hermes_ofdm {

    gi_adder::sptr
    gi_adder::make(int Nfft, int Ngi)
    {
      return gnuradio::get_initial_sptr
        (new gi_adder_impl(Nfft, Ngi));
    }

    /*
     * The private constructor
     */
    gi_adder_impl::gi_adder_impl(int Nfft, int Ngi)
      : gr::block("gi_adder",
              gr::io_signature::make(1, 1, sizeof(gr_complex)*Nfft),
              gr::io_signature::make(1, 1, sizeof(gr_complex)*(Nfft+Ngi)))
    {
    	this->Nfft=Nfft;
    	this->Ngi=Ngi;
    }

    /*
     * Our virtual destructor.
     */
    gi_adder_impl::~gi_adder_impl()
    {
    }

    void
    gi_adder_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
    {
      ninput_items_required[0] = noutput_items;
    }

    int
    gi_adder_impl::general_work (int noutput_items,
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
        	  out[i*(Nfft+Ngi)+j+Ngi]=in[i*Nfft+j];
          }
          for(int j=0;j<Ngi;j++)
          {
        	  out[i*(Nfft+Ngi)+j]=in[i*Nfft+j+Nfft-Ngi];
          }
      }

      consume_each (noutput_items);
      return noutput_items;
    }

  } /* namespace hermes_ofdm */
} /* namespace gr */

