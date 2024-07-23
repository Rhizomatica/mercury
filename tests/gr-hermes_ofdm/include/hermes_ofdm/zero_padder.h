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


#ifndef INCLUDED_HERMES_OFDM_ZERO_PADDER_H
#define INCLUDED_HERMES_OFDM_ZERO_PADDER_H

#include <hermes_ofdm/api.h>
#include <gnuradio/block.h>

namespace gr {
  namespace hermes_ofdm {

    /*!
     * \brief <+description of block+>
     * \ingroup hermes_ofdm
     *
     */
    class HERMES_OFDM_API zero_padder : virtual public gr::block
    {
     public:
      typedef boost::shared_ptr<zero_padder> sptr;

      /*!
       * \brief Return a shared_ptr to a new instance of hermes_ofdm::zero_padder.
       *
       * To avoid accidental use of raw pointers, hermes_ofdm::zero_padder's
       * constructor is in a private implementation
       * class. hermes_ofdm::zero_padder::make is the public interface for
       * creating new instances.
       */
      static sptr make(int Nc, int Nfft);
    };

  } // namespace hermes_ofdm
} // namespace gr

#endif /* INCLUDED_HERMES_OFDM_ZERO_PADDER_H */

