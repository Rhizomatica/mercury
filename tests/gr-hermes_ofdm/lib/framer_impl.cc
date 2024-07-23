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
#include "framer_impl.h"

namespace gr {
  namespace hermes_ofdm {

    framer::sptr
    framer::make(int Nc, int Nsymb, int Ndata, int Dx, int Dy, int pilot_mod, int edge_pilots, int config_bits)
    {
      return gnuradio::get_initial_sptr
        (new framer_impl(Nc, Nsymb, Ndata, Dx, Dy, pilot_mod, edge_pilots, config_bits));
    }

    /*
     * The private constructor
     */
    framer_impl::framer_impl(int Nc, int Nsymb, int Ndata, int Dx, int Dy, int pilot_mod, int edge_pilots, int config_bits)
      : gr::block("framer",
              gr::io_signature::make(1, 1, sizeof(gr_complex)*Ndata),
              gr::io_signature::make(1, 1, sizeof(gr_complex)*(Nc*Nsymb)))
    {
        this->Nc=Nc;
        this->Nsymb=Nsymb;
        this->Ndata=Ndata;
        this->Dx=Dx;
        this->Dy=Dy;
        this->pilot_mod=pilot_mod;
        this->edge_pilots=edge_pilots;
        this->config_bits=config_bits;

        ofdm_frame = new struct carrier[this->Nsymb*this->Nc];



        for(int j=0;j<this->Nsymb;j++)
        {
        	for(int i=0;i<this->Nc;i++)
        	{
        		(ofdm_frame+j*this->Nc+i)->type=DATA;
        	}
        }

        framer_pilot_configurator pilot_configurator (this->Nc,this->Nsymb);
        pilot_configurator.Dx=Dx;
        pilot_configurator.Dx_start=pilot_configurator.Dx;
        pilot_configurator.Dy=Dy;
        pilot_configurator.first_col=PILOT;
        pilot_configurator.last_col=PILOT;
        pilot_configurator.carrier=ofdm_frame;
        if(Dx!=0 && Dy!=0)
        {
        	pilot_configurator.configure();
        }
        if(pilot_configurator.nData!=Ndata)
        {
        	std::cout<<"Framer:Wrond data size, nData has to be="<<pilot_configurator.nData<<std::endl;
        	exit(0);
        }



    }

    /*
     * Our virtual destructor.
     */
    framer_impl::~framer_impl()
    {
    	delete ofdm_frame;
    }

    void
    framer_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
    {
    	ninput_items_required[0] = noutput_items;
    }

    int
    framer_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
        const gr_complex *in = (const gr_complex *) input_items[0];
        gr_complex *out = (gr_complex *) output_items[0];

        int data_index;
        int last_pilot=0;
        int pilot_value;

        for(int i=0;i<noutput_items;i++)
        {
        	srand(0);
        	data_index=0;
        	last_pilot=0;
        	for(int j=0;j<Nsymb;j++)
        	{
        		for(int k=0;k<Nc;k++)
        		{
        			if((ofdm_frame+j*this->Nc+k)->type==DATA)
        			{
        				out[i*(Nsymb*Nc)+j*Nc+k]=in[i*Ndata+data_index];
        				data_index++;
        			}
        			else if ((ofdm_frame+j*this->Nc+k)->type==PILOT && this->pilot_mod==DBPSK)
        			{
        				pilot_value=rand()%2 ^ last_pilot;
        				out[i*(Nsymb*Nc)+j*Nc+k]=gr_complex(2*pilot_value-1,0);
        				last_pilot=pilot_value;
        			}
        		}
            }

        }


      consume_each (noutput_items);
      return noutput_items;
    }


    framer_pilot_configurator::framer_pilot_configurator(int Nc, int Nsymb)
    {


    	first_col=DATA;
    	last_col=DATA;
    	second_col=DATA;
    	this->Nc=Nc;
    	this->Nsymb=Nsymb;
    	if(Nc>Nsymb)
    	{
    		this->Nc_max=Nc;
    	}
    	else
    	{
    		this->Nc_max=Nsymb;
    	}
    	nData=Nc*Nsymb;
    	nPilots=0;
    	Dx_start=0;
    	nConfig=0;

    	carrier=0;
    	Dy=0;
    	Dx=0;



    	virtual_carrier = new struct carrier[this->Nc_max*this->Nc_max];


        for(int j=0;j<this->Nc_max;j++)
        {
        	for(int i=0;i<this->Nc_max;i++)
        	{
        		(virtual_carrier+j*this->Nc_max+i)->type=DATA;
        	}

        }
    }

    framer_pilot_configurator::~framer_pilot_configurator()
    {

    }

    void framer_pilot_configurator::configure()
    {
    	int x=Dx_start;
    	int y=0;

    	while(x<Nc_max && y<Nc_max)
    	{
    		(virtual_carrier+y*Nc_max+x)->type=PILOT;

    		for(int j=y;j<Nc_max;j+=Dy)
    		{
    			(virtual_carrier+j*Nc_max+x)->type=PILOT;
    		}
    		for(int j=y;j>=0;j-=Dy)
    		{
    			(virtual_carrier+j*Nc_max+x)->type=PILOT;
    		}

    		y++;
    		x+=Dx;

    	}

    	for(int j=0;j<Nc_max;j++)
    	{
    		if(first_col==PILOT)
    		{
    			(virtual_carrier+j*Nc_max+0)->type=PILOT;
    		}
    		if(last_col==PILOT)
    		{
    			(virtual_carrier+j*Nc_max+Nc-1)->type=PILOT;
    		}
    		if(second_col==CONFIG&&(virtual_carrier+j*Nc_max+1)->type!=PILOT)
    		{
    			(virtual_carrier+j*Nc_max+1)->type=CONFIG;
    		}
    	}

    	for(int j=0;j<Nsymb;j++)
    	{
    		for(int i=0;i<Nc;i++)
    		{
    			if((virtual_carrier+j*Nc_max+i)->type!=DATA)
    			{
    				(carrier + j*Nc+i)->type=(virtual_carrier+j*Nc_max+i)->type;
    			}
    			if((virtual_carrier+j*Nc_max+i)->type==PILOT)
    			{
    				nPilots++;
    			}
    			if((virtual_carrier+j*Nc_max+i)->type==CONFIG)
    			{
    				nConfig++;
    			}

    		}

    	}
    	nData-=nPilots+nConfig;
    }

  } /* namespace hermes_ofdm */
} /* namespace gr */

