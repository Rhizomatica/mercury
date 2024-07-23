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


#ifndef INCLUDED_HERMES_OFDM_GI_ADDER_H
#define INCLUDED_HERMES_OFDM_GI_ADDER_H

#include <hermes_ofdm/api.h>
#include <gnuradio/block.h>

namespace gr {
  namespace hermes_ofdm {

    /*!
     * \brief <+description of block+>
     * \ingroup hermes_ofdm
     *
     */
    class HERMES_OFDM_API gi_adder : virtual public gr::block
    {
     public:
      typedef boost::shared_ptr<gi_adder> sptr;

      /*!
       * \brief Return a shared_ptr to a new instance of hermes_ofdm::gi_adder.
       *
       * To avoid accidental use of raw pointers, hermes_ofdm::gi_adder's
       * constructor is in a private implementation
       * class. hermes_ofdm::gi_adder::make is the public interface for
       * creating new instances.
       */
      static sptr make(int Nfft, int Ngi);
    };

  } // namespace hermes_ofdm
} // namespace gr

#endif /* INCLUDED_HERMES_OFDM_GI_ADDER_H */

