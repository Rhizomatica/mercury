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

#ifndef INCLUDED_HERMES_OFDM_DEFRAMER_IMPL_H
#define INCLUDED_HERMES_OFDM_DEFRAMER_IMPL_H

#include <hermes_ofdm/deframer.h>
#include <complex>
#include <cmath>


#define DATA 0
#define PILOT 1
#define CONFIG 2

#define DBPSK 0


namespace gr {
  namespace hermes_ofdm {

  struct carrier
  {
  	std::complex <double> value;
  	int type;

  };

    class deframer_impl : public deframer
    {
     private:
        int Nc, Nsymb, Ndata, Dx, Dy, pilot_mod, edge_pilots;


     public:
      deframer_impl(int Nc, int Nsymb, int Ndata, int Dx, int Dy, int pilot_mod, int edge_pilots);
      ~deframer_impl();
      struct carrier* ofdm_frame;
      std::complex <double>* h;

      // Where all the action really happens
      void forecast (int noutput_items, gr_vector_int &ninput_items_required);

      int general_work(int noutput_items,
           gr_vector_int &ninput_items,
           gr_vector_const_void_star &input_items,
           gr_vector_void_star &output_items);
    };

    class deframer_pilot_configurator
    {

    public:
    	deframer_pilot_configurator(int Nc, int Nsymb);
    	~deframer_pilot_configurator();
    	void configure();
    	struct carrier* carrier;
    	int Dx,Dy;
    	int Dx_start;
    	int first_col,second_col,last_col;
    	int nData,nPilots,nConfig;
    	int Nc, Nsymb, Nc_max;

    private:
    	struct carrier* virtual_carrier;

    };

  } // namespace hermes_ofdm
} // namespace gr

#endif /* INCLUDED_HERMES_OFDM_DEFRAMER_IMPL_H */

