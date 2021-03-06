/*
 * Copyright (c) 2008 Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

 /* Implementation of beacon processing. */

#include "core.h"

/*
 *  Configure parameters for the beacon queue
 *
 *  This function will modify certain transmit queue properties depending on
 *  the operating mode of the station (AP or AdHoc).  Parameters are AIFS
 *  settings and channel width min/max
*/

static int ath_beaconq_config(struct ath_softc *sc)
{
	struct ath_hal *ah = sc->sc_ah;
	struct hal_txq_info qi;

	ath9k_hw_gettxqueueprops(ah, sc->sc_bhalq, &qi);
	if (sc->sc_opmode == HAL_M_HOSTAP) {
		/* Always burst out beacon and CAB traffic. */
		qi.tqi_aifs = 1;
		qi.tqi_cwmin = 0;
		qi.tqi_cwmax = 0;
	} else {
		/* Adhoc mode; important thing is to use 2x cwmin. */
		qi.tqi_aifs = sc->sc_beacon_qi.tqi_aifs;
		qi.tqi_cwmin = 2*sc->sc_beacon_qi.tqi_cwmin;
		qi.tqi_cwmax = sc->sc_beacon_qi.tqi_cwmax;
	}

	if (!ath9k_hw_settxqueueprops(ah, sc->sc_bhalq, &qi)) {
		DPRINTF(sc, ATH_DEBUG_FATAL,
			"%s: unable to update h/w beacon queue parameters\n",
			__func__);
		return 0;
	} else {
		ath9k_hw_resettxqueue(ah, sc->sc_bhalq); /* push to h/w */
		return 1;
	}
}

/*
 *  Setup the beacon frame for transmit.
 *
 *  Associates the beacon frame buffer with a transmit descriptor.  Will set
 *  up all required antenna switch parameters, rate codes, and channel flags.
 *  Beacons are always sent out at the lowest rate, and are not retried.
*/

static void ath_beacon_setup(struct ath_softc *sc,
	struct ath_vap *avp, struct ath_buf *bf)
{
	struct sk_buff *skb = (struct sk_buff *)bf->bf_mpdu;
	struct ath_hal *ah = sc->sc_ah;
	struct ath_desc *ds;
	int flags, antenna;
	const struct hal_rate_table *rt;
	u_int8_t rix, rate;
	int ctsrate = 0;
	int ctsduration = 0;
	struct hal_11n_rate_series  series[4];

	DPRINTF(sc, ATH_DEBUG_BEACON, "%s: m %p len %u\n",
		__func__, skb, skb->len);

	/* setup descriptors */
	ds = bf->bf_desc;

	flags = HAL_TXDESC_NOACK;

	if (sc->sc_opmode == HAL_M_IBSS && sc->sc_hasveol) {
		ds->ds_link = bf->bf_daddr; /* self-linked */
		flags |= HAL_TXDESC_VEOL;
		/* Let hardware handle antenna switching. */
		antenna = 0;
	} else {
		ds->ds_link = 0;
		/*
		 * Switch antenna every beacon.
		 * Should only switch every beacon period, not for every
		 * SWBA's
		 * XXX assumes two antenna
		 */
		if (sc->sc_stagbeacons)
			antenna = ((sc->ast_be_xmit /
					sc->sc_nbcnvaps) & 1 ? 2 : 1);
		else
			antenna = (sc->ast_be_xmit & 1 ? 2 : 1);
	}

	ds->ds_data = bf->bf_buf_addr;

	/*
	 * Calculate rate code.
	 * XXX everything at min xmit rate
	 */
	rix = sc->sc_minrateix;
	rt = sc->sc_currates;
	rate = rt->info[rix].rateCode;
	if (sc->sc_flags & ATH_PREAMBLE_SHORT)
		rate |= rt->info[rix].shortPreamble;

	ath9k_hw_set11n_txdesc(ah, ds
			      , skb->len + FCS_LEN /* frame length */
			      , HAL_PKT_TYPE_BEACON /* Atheros packet type */
			      , avp->av_btxctl.txpower /* txpower XXX */
			      , HAL_TXKEYIX_INVALID /* no encryption */
			      , HAL_KEY_TYPE_CLEAR /* no encryption */
			      , flags /* no ack, veol for beacons */
		);

	/* NB: beacon's BufLen must be a multiple of 4 bytes */
	ath9k_hw_filltxdesc(ah, ds
			   , roundup(skb->len, 4) /* buffer length */
			   , AH_TRUE /* first segment */
			   , AH_TRUE /* last segment */
			   , ds /* first descriptor */
		);

	memzero(series, sizeof(struct hal_11n_rate_series) * 4);
	series[0].Tries = 1;
	series[0].Rate = rate;
	series[0].ChSel = sc->sc_tx_chainmask;
	series[0].RateFlags = (ctsrate) ? HAL_RATESERIES_RTS_CTS : 0;
	ath9k_hw_set11n_ratescenario(ah, ds, ds, 0,
		ctsrate, ctsduration, series, 4, 0);

	/* NB: The desc swap function becomes void,
	 * if descriptor swapping is not enabled
	 */
	ath_desc_swap(ds);
}

/* Move everything from the vap's mcast queue to the hardware cab queue.
 * Caller must hold mcasq lock and cabq lock
 * XXX MORE_DATA bit?
 */
static void empty_mcastq_into_cabq(struct ath_hal *ah,
	struct ath_txq *mcastq, struct ath_txq *cabq)
{
	struct ath_buf *bfmcast;

	BUG_ON(list_empty(&mcastq->axq_q));

	bfmcast = list_first_entry(&mcastq->axq_q, struct ath_buf, list);

	/* link the descriptors */
	if (!cabq->axq_link)
		ath9k_hw_puttxbuf(ah, cabq->axq_qnum, bfmcast->bf_daddr);
	else
		*cabq->axq_link = cpu_to_le32(bfmcast->bf_daddr);

	/* append the private vap mcast list to  the cabq */

	cabq->axq_depth	+= mcastq->axq_depth;
	cabq->axq_totalqueued += mcastq->axq_totalqueued;
	cabq->axq_linkbuf = mcastq->axq_linkbuf;
	cabq->axq_link = mcastq->axq_link;
	list_splice_tail_init(&mcastq->axq_q, &cabq->axq_q);
	mcastq->axq_depth = 0;
	mcastq->axq_totalqueued = 0;
	mcastq->axq_linkbuf = NULL;
	mcastq->axq_link = NULL;
}

/* This is only run at DTIM. We move everything from the vap's mcast queue
 * to the hardware cab queue. Caller must hold the mcastq lock. */
static void trigger_mcastq(struct ath_hal *ah,
	struct ath_txq *mcastq, struct ath_txq *cabq)
{
	spin_lock_bh(&cabq->axq_lock);

	if (!list_empty(&mcastq->axq_q))
		empty_mcastq_into_cabq(ah, mcastq, cabq);

	/* cabq is gated by beacon so it is safe to start here */
	if (!list_empty(&cabq->axq_q))
		ath9k_hw_txstart(ah, cabq->axq_qnum);

	spin_unlock_bh(&cabq->axq_lock);
}

/*
 *  Generate beacon frame and queue cab data for a vap.
 *
 *  Updates the contents of the beacon frame.  It is assumed that the buffer for
 *  the beacon frame has been allocated in the ATH object, and simply needs to
 *  be filled for this cycle.  Also, any CAB (crap after beacon?) traffic will
 *  be added to the beacon frame at this point.
*/
static struct ath_buf *ath_beacon_generate(struct ath_softc *sc, int if_id)
{
	struct ath_hal *ah = sc->sc_ah;
	struct ath_buf *bf;
	struct ath_vap *avp;
	struct sk_buff *skb;
	int cabq_depth;
	int mcastq_depth;
	int is_beacon_dtim = 0;
	unsigned int curlen;
	struct ath_txq *cabq;
	struct ath_txq *mcastq;
	avp = sc->sc_vaps[if_id];

	mcastq = &avp->av_mcastq;
	cabq = sc->sc_cabq;

	ASSERT(avp);

	if (avp->av_bcbuf == NULL) {
		DPRINTF(sc, ATH_DEBUG_BEACON, "%s: avp=%p av_bcbuf=%p\n",
			__func__, avp, avp->av_bcbuf);
		return NULL;
	}
	bf = avp->av_bcbuf;
	skb = (struct sk_buff *) bf->bf_mpdu;

	/*
	 * Update dynamic beacon contents.  If this returns
	 * non-zero then we need to remap the memory because
	 * the beacon frame changed size (probably because
	 * of the TIM bitmap).
	 */
	curlen = skb->len;

	/* XXX: spin_lock_bh should not be used here, but sparse bitches
	 * otherwise. We should fix sparse :) */
	spin_lock_bh(&mcastq->axq_lock);
	mcastq_depth = avp->av_mcastq.axq_depth;

	if (ath_update_beacon(sc, if_id, &avp->av_boff, skb, mcastq_depth) ==
	    1) {
		ath_skb_unmap_single(sc, skb, PCI_DMA_TODEVICE,
				     get_dma_mem_context(bf, bf_dmacontext));
		bf->bf_buf_addr = ath_skb_map_single(sc, skb, PCI_DMA_TODEVICE,
			get_dma_mem_context(bf, bf_dmacontext));
	} else {
		pci_dma_sync_single_for_cpu(sc->pdev,
					    bf->bf_buf_addr,
					    skb_tailroom(skb),
					    PCI_DMA_TODEVICE);
	}

	/*
	 * if the CABQ traffic from previous DTIM is pending and the current
	 *  beacon is also a DTIM.
	 *  1) if there is only one vap let the cab traffic continue.
	 *  2) if there are more than one vap and we are using staggered
	 *     beacons, then drain the cabq by dropping all the frames in
	 *     the cabq so that the current vaps cab traffic can be scheduled.
	 */
	spin_lock_bh(&cabq->axq_lock);
	cabq_depth = cabq->axq_depth;
	spin_unlock_bh(&cabq->axq_lock);

	is_beacon_dtim = avp->av_boff.bo_tim[4] & 1;

	if (mcastq_depth && is_beacon_dtim && cabq_depth) {
		/*
		 * Unlock the cabq lock as ath_tx_draintxq acquires
		 * the lock again which is a common function and that
		 * acquires txq lock inside.
		 */
		if (sc->sc_nvaps > 1 && sc->sc_stagbeacons) {
			ath_tx_draintxq(sc, cabq, AH_FALSE);
			DPRINTF(sc, ATH_DEBUG_BEACON,
				"%s: flush previous cabq traffic\n", __func__);
		}
	}

	/* Construct tx descriptor. */
	ath_beacon_setup(sc, avp, bf);

	/*
	 * Enable the CAB queue before the beacon queue to
	 * insure cab frames are triggered by this beacon.
	 */
	if (is_beacon_dtim)
		trigger_mcastq(ah, mcastq, cabq);

	spin_unlock_bh(&mcastq->axq_lock);
	return bf;
}

/*
 * Startup beacon transmission for adhoc mode when they are sent entirely
 * by the hardware using the self-linked descriptor + veol trick.
*/

static void ath_beacon_start_adhoc(struct ath_softc *sc, int if_id)
{
	struct ath_hal *ah = sc->sc_ah;
	struct ath_buf *bf;
	struct ath_vap *avp;
	struct sk_buff *skb;

	avp = sc->sc_vaps[if_id];
	ASSERT(avp);

	if (avp->av_bcbuf == NULL) {
		DPRINTF(sc, ATH_DEBUG_BEACON, "%s: avp=%p av_bcbuf=%p\n",
			__func__, avp, avp != NULL ? avp->av_bcbuf : NULL);
		return;
	}
	bf = avp->av_bcbuf;
	skb = (struct sk_buff *) bf->bf_mpdu;

	/* Construct tx descriptor. */
	ath_beacon_setup(sc, avp, bf);

	/* NB: caller is known to have already stopped tx dma */
	ath9k_hw_puttxbuf(ah, sc->sc_bhalq, bf->bf_daddr);
	ath9k_hw_txstart(ah, sc->sc_bhalq);
	DPRINTF(sc, ATH_DEBUG_BEACON, "%s: TXDP%u = %llx (%p)\n", __func__,
		sc->sc_bhalq, ito64(bf->bf_daddr), bf->bf_desc);
}

/*
 *  Setup a h/w transmit queue for beacons.
 *
 *  This function allocates an information structure (struct hal_txq_info)
 *  on the stack, sets some specific parameters (zero out channel width
 *  min/max, and enable aifs). The info structure does not need to be
 *  persistant.
*/

int ath_beaconq_setup(struct ath_hal *ah)
{
	struct hal_txq_info qi;

	memzero(&qi, sizeof(qi));
	qi.tqi_aifs = 1;
	qi.tqi_cwmin = 0;
	qi.tqi_cwmax = 0;
	/* NB: don't enable any interrupts */
	return ath9k_hw_setuptxqueue(ah, HAL_TX_QUEUE_BEACON, &qi);
}


/*
 *  Allocate and setup an initial beacon frame.
 *
 *  Allocate a beacon state variable for a specific VAP instance created on
 *  the ATH interface.  This routine also calculates the beacon "slot" for
 *  staggared beacons in the mBSSID case.
*/

int ath_beacon_alloc(struct ath_softc *sc, int if_id)
{
	struct ath_vap *avp;
	struct ieee80211_hdr *wh;
	struct ath_buf *bf;
	struct sk_buff *skb;

	avp = sc->sc_vaps[if_id];
	ASSERT(avp);

	/* Allocate a beacon descriptor if we haven't done so. */
	if (!avp->av_bcbuf) {
		/*
		 * Allocate beacon state for hostap/ibss.  We know
		 * a buffer is available.
		 */

		avp->av_bcbuf = list_first_entry(&sc->sc_bbuf,
				struct ath_buf, list);
		list_del(&avp->av_bcbuf->list);

		if (sc->sc_opmode == HAL_M_HOSTAP || !sc->sc_hasveol) {
			int slot;
			/*
			 * Assign the vap to a beacon xmit slot. As
			 * above, this cannot fail to find one.
			 */
			avp->av_bslot = 0;
			for (slot = 0; slot < ATH_BCBUF; slot++)
				if (sc->sc_bslot[slot] == ATH_IF_ID_ANY) {
					/*
					 * XXX hack, space out slots to better
					 * deal with misses
					 */
					if (slot+1 < ATH_BCBUF &&
					    sc->sc_bslot[slot+1] ==
						ATH_IF_ID_ANY) {
						avp->av_bslot = slot+1;
						break;
					}
					avp->av_bslot = slot;
					/* NB: keep looking for a double slot */
				}
			KASSERT(sc->sc_bslot[avp->av_bslot] == ATH_IF_ID_ANY,
				("beacon slot %u not empty?", avp->av_bslot));
			sc->sc_bslot[avp->av_bslot] = if_id;
			sc->sc_nbcnvaps++;
		}
	}

	/* release the previous beacon frame , if it already exists. */
	bf = avp->av_bcbuf;
	if (bf->bf_mpdu != NULL) {
		struct ath_xmit_status tx_status;

		skb = (struct sk_buff *) bf->bf_mpdu;
		ath_skb_unmap_single(sc, skb, PCI_DMA_TODEVICE,
				     get_dma_mem_context(bf, bf_dmacontext));
		tx_status.flags = 0;
		tx_status.retries = 0;
		ath_tx_complete(sc, skb, &tx_status, NULL);
		bf->bf_mpdu = NULL;
	}

	/*
	 * NB: the beacon data buffer must be 32-bit aligned;
	 * we assume the wbuf routines will return us something
	 * with this alignment (perhaps should assert).
	 */
	skb = ath_get_beacon(sc, if_id, &avp->av_boff, &avp->av_btxctl);
	if (skb == NULL) {
		DPRINTF(sc, ATH_DEBUG_BEACON, "%s: cannot get skb\n",
			__func__);
		return -ENOMEM;
	}

	/*
	 * Calculate a TSF adjustment factor required for
	 * staggered beacons.  Note that we assume the format
	 * of the beacon frame leaves the tstamp field immediately
	 * following the header.
	 */
	if (sc->sc_stagbeacons && avp->av_bslot > 0) {
		u_int64_t tsfadjust;
		int intval;

		/* FIXME: Use default value for now: Sujith */

		intval = ATH_DEFAULT_BINTVAL;

		/*
		 * The beacon interval is in TU's; the TSF in usecs.
		 * We figure out how many TU's to add to align the
		 * timestamp then convert to TSF units and handle
		 * byte swapping before writing it in the frame.
		 * The hardware will then add this each time a beacon
		 * frame is sent.  Note that we align vap's 1..N
		 * and leave vap 0 untouched.  This means vap 0
		 * has a timestamp in one beacon interval while the
		 * others get a timestamp aligned to the next interval.
		 */
		tsfadjust = (intval * (ATH_BCBUF - avp->av_bslot)) / ATH_BCBUF;
		tsfadjust = cpu_to_le64(tsfadjust<<10);     /* TU->TSF */

		DPRINTF(sc, ATH_DEBUG_BEACON,
			"%s: %s beacons, bslot %d intval %u tsfadjust %llu\n",
			__func__, sc->sc_stagbeacons ? "stagger" : "burst",
			avp->av_bslot, intval, (unsigned long long)tsfadjust);

		wh = (struct ieee80211_hdr *)skb->data;
		memcpy(&wh[1], &tsfadjust, sizeof(tsfadjust));
	}

	bf->bf_buf_addr = ath_skb_map_single(sc, skb, PCI_DMA_TODEVICE,
		get_dma_mem_context(bf, bf_dmacontext));
	bf->bf_mpdu = skb;

	return 0;
}

/*
 *  Reclaim beacon resources and return buffer to the pool.
 *
 *  Checks the VAP to put the beacon frame buffer back to the ATH object
 *  queue, and de-allocates any wbuf frames that were sent as CAB traffic.
*/

void ath_beacon_return(struct ath_softc *sc, struct ath_vap *avp)
{
	if (avp->av_bcbuf != NULL) {
		struct ath_buf *bf;

		if (avp->av_bslot != -1) {
			sc->sc_bslot[avp->av_bslot] = ATH_IF_ID_ANY;
			sc->sc_nbcnvaps--;
		}

		bf = avp->av_bcbuf;
		if (bf->bf_mpdu != NULL) {
			struct sk_buff *skb = (struct sk_buff *) bf->bf_mpdu;
			struct ath_xmit_status tx_status;

			ath_skb_unmap_single(sc, skb, PCI_DMA_TODEVICE,
				get_dma_mem_context(bf, bf_dmacontext));
			tx_status.flags = 0;
			tx_status.retries = 0;
			ath_tx_complete(sc, skb, &tx_status, NULL);
			bf->bf_mpdu = NULL;
		}
		list_add_tail(&bf->list, &sc->sc_bbuf);

		avp->av_bcbuf = NULL;
	}
}

/*
 *  Reclaim beacon resources and return buffer to the pool.
 *
 *  This function will free any wbuf frames that are still attached to the
 *  beacon buffers in the ATH object.  Note that this does not de-allocate
 *  any wbuf objects that are in the transmit queue and have not yet returned
 *  to the ATH object.
*/

void ath_beacon_free(struct ath_softc *sc)
{
	struct ath_buf *bf;

	list_for_each_entry(bf, &sc->sc_bbuf, list) {
		if (bf->bf_mpdu != NULL) {
			struct sk_buff *skb = (struct sk_buff *) bf->bf_mpdu;
			struct ath_xmit_status tx_status;

			ath_skb_unmap_single(sc, skb, PCI_DMA_TODEVICE,
				get_dma_mem_context(bf, bf_dmacontext));
			tx_status.flags = 0;
			tx_status.retries = 0;
			ath_tx_complete(sc, skb, &tx_status, NULL);
			bf->bf_mpdu = NULL;
		}
	}
}

/*
 * Tasklet for Sending Beacons
 *
 * Transmit one or more beacon frames at SWBA.  Dynamic updates to the frame
 * contents are done as needed and the slot time is also adjusted based on
 * current state.
 *
 * This tasklet is not scheduled, it's called in ISR context.
*/

void ath9k_beacon_tasklet(unsigned long data)
{
#define TSF_TO_TU(_h,_l)					\
	((((u_int32_t)(_h)) << 22) | (((u_int32_t)(_l)) >> 10))

	struct ath_softc *sc = (struct ath_softc *)data;
	struct ath_hal *ah = sc->sc_ah;
	struct ath_buf *bf = NULL;
	int slot, if_id;
	u_int32_t bfaddr;
	u_int32_t rx_clear = 0, rx_frame = 0, tx_frame = 0;
	u_int32_t show_cycles = 0;
	u_int32_t bc = 0; /* beacon count */

	if (sc->sc_noreset) {
		show_cycles = ath9k_hw_GetMibCycleCountsPct(ah,
							    &rx_clear,
							    &rx_frame,
							    &tx_frame);
	}

	/*
	 * Check if the previous beacon has gone out.  If
	 * not don't try to post another, skip this period
	 * and wait for the next.  Missed beacons indicate
	 * a problem and should not occur.  If we miss too
	 * many consecutive beacons reset the device.
	 */
	if (ath9k_hw_numtxpending(ah, sc->sc_bhalq) != 0) {
		sc->sc_bmisscount++;
		/* XXX: doth needs the chanchange IE countdown decremented.
		 *      We should consider adding a mac80211 call to indicate
		 *      a beacon miss so appropriate action could be taken
		 *      (in that layer).
		 */
		if (sc->sc_bmisscount < BSTUCK_THRESH) {
			if (sc->sc_noreset) {
				DPRINTF(sc, ATH_DEBUG_BEACON,
					"%s: missed %u consecutive beacons\n",
					__func__, sc->sc_bmisscount);
				if (show_cycles) {
					/*
					 * Display cycle counter stats
					 * from HW to aide in debug of
					 * stickiness.
					 */
					DPRINTF(sc,
						ATH_DEBUG_BEACON,
						"%s: busy times: rx_clear=%d, "
						"rx_frame=%d, tx_frame=%d\n",
						__func__, rx_clear, rx_frame,
						tx_frame);
				} else {
					DPRINTF(sc,
						ATH_DEBUG_BEACON,
						"%s: unable to obtain "
						"busy times\n", __func__);
				}
			} else {
				DPRINTF(sc, ATH_DEBUG_BEACON,
					"%s: missed %u consecutive beacons\n",
					__func__, sc->sc_bmisscount);
			}
		} else if (sc->sc_bmisscount >= BSTUCK_THRESH) {
			if (sc->sc_noreset) {
				if (sc->sc_bmisscount == BSTUCK_THRESH) {
					DPRINTF(sc,
						ATH_DEBUG_BEACON,
						"%s: beacon is officially "
						"stuck\n", __func__);
					ath9k_hw_dmaRegDump(ah);
				}
			} else {
				DPRINTF(sc, ATH_DEBUG_BEACON,
					"%s: beacon is officially stuck\n",
					__func__);
				ath_bstuck_process(sc);
			}
		}

		return;
	}
	if (sc->sc_bmisscount != 0) {
		if (sc->sc_noreset) {
			DPRINTF(sc,
				ATH_DEBUG_BEACON,
				"%s: resume beacon xmit after %u misses\n",
				__func__, sc->sc_bmisscount);
		} else {
			DPRINTF(sc, ATH_DEBUG_BEACON,
				"%s: resume beacon xmit after %u misses\n",
				__func__, sc->sc_bmisscount);
		}
		sc->sc_bmisscount = 0;
	}

	/*
	 * Generate beacon frames.  If we are sending frames
	 * staggered then calculate the slot for this frame based
	 * on the tsf to safeguard against missing an swba.
	 * Otherwise we are bursting all frames together and need
	 * to generate a frame for each vap that is up and running.
	 */
	if (sc->sc_stagbeacons) {
		/* staggered beacons */
		u_int64_t tsf;
		u_int32_t tsftu;
		u_int16_t intval;

		/* FIXME: Use default value for now - Sujith */
		intval = ATH_DEFAULT_BINTVAL;

		tsf = ath9k_hw_gettsf64(ah);
		tsftu = TSF_TO_TU(tsf>>32, tsf);
		slot = ((tsftu % intval) * ATH_BCBUF) / intval;
		if_id = sc->sc_bslot[(slot + 1) % ATH_BCBUF];
		DPRINTF(sc, ATH_DEBUG_BEACON,
			"%s: slot %d [tsf %llu tsftu %u intval %u] if_id %d\n",
			__func__, slot, (unsigned long long) tsf, tsftu,
			intval, if_id);
		bfaddr = 0;
		if (if_id != ATH_IF_ID_ANY) {
			bf = ath_beacon_generate(sc, if_id);
			if (bf != NULL) {
				bfaddr = bf->bf_daddr;
				bc = 1;
			}
		}
	} else {
		/* XXX: Clean this up, move work to a helper */
		/* burst'd beacons */
		u_int32_t *bflink;
		bflink = &bfaddr;
		/* XXX rotate/randomize order? */
		for (slot = 0; slot < ATH_BCBUF; slot++) {
			if_id = sc->sc_bslot[slot];
			if (if_id != ATH_IF_ID_ANY) {
				bf = ath_beacon_generate(sc, if_id);
				if (bf != NULL) {
					if (bflink != &bfaddr)
						*bflink = cpu_to_le32(
							bf->bf_daddr);
					else
						*bflink = bf->bf_daddr;
					bflink = &bf->bf_desc->ds_link;
					bc++;
				}
			}
		}
		*bflink = 0;    /* link of last frame */
	}
	/*
	 * Handle slot time change when a non-ERP station joins/leaves
	 * an 11g network.  The 802.11 layer notifies us via callback,
	 * we mark updateslot, then wait one beacon before effecting
	 * the change.  This gives associated stations at least one
	 * beacon interval to note the state change.
	 *
	 * NB: The slot time change state machine is clocked according
	 *     to whether we are bursting or staggering beacons.  We
	 *     recognize the request to update and record the current
	 *     slot then don't transition until that slot is reached
	 *     again.  If we miss a beacon for that slot then we'll be
	 *     slow to transition but we'll be sure at least one beacon
	 *     interval has passed.  When bursting slot is always left
	 *     set to ATH_BCBUF so this check is a noop.
	 */
	/* XXX locking */
	if (sc->sc_updateslot == UPDATE) {
		sc->sc_updateslot = COMMIT; /* commit next beacon */
		sc->sc_slotupdate = slot;
	} else if (sc->sc_updateslot == COMMIT && sc->sc_slotupdate == slot)
		ath_setslottime(sc);        /* commit change to hardware */

	if ((!sc->sc_stagbeacons || slot == 0) && (!sc->sc_diversity)) {
		int otherant;
		/*
		 * Check recent per-antenna transmit statistics and flip
		 * the default rx antenna if noticeably more frames went out
		 * on the non-default antenna.  Only do this if rx diversity
		 * is off.
		 * XXX assumes 2 anntenae
		 */
		otherant = sc->sc_defant & 1 ? 2 : 1;
		if (sc->sc_ant_tx[otherant] > sc->sc_ant_tx[sc->sc_defant] +
			ATH_ANTENNA_DIFF) {
			DPRINTF(sc, ATH_DEBUG_BEACON,
				"%s: flip defant to %u, %u > %u\n",
				__func__, otherant, sc->sc_ant_tx[otherant],
				sc->sc_ant_tx[sc->sc_defant]);
			ath_setdefantenna(sc, otherant);
		}
		sc->sc_ant_tx[1] = sc->sc_ant_tx[2] = 0;
	}

	if (bfaddr != 0) {
		/*
		 * Stop any current dma and put the new frame(s) on the queue.
		 * This should never fail since we check above that no frames
		 * are still pending on the queue.
		 */
		if (!ath9k_hw_stoptxdma(ah, sc->sc_bhalq)) {
			DPRINTF(sc, ATH_DEBUG_FATAL,
				"%s: beacon queue %u did not stop?\n",
				__func__, sc->sc_bhalq);
			/* NB: the HAL still stops DMA, so proceed */
		}

		/* NB: cabq traffic should already be queued and primed */
		ath9k_hw_puttxbuf(ah, sc->sc_bhalq, bfaddr);
		ath9k_hw_txstart(ah, sc->sc_bhalq);

		sc->ast_be_xmit += bc;     /* XXX per-vap? */
	}
#undef TSF_TO_TU
}

/*
 *  Tasklet for Beacon Stuck processing
 *
 *  Processing for Beacon Stuck.
 *  Basically calls the ath_internal_reset function to reset the chip.
*/

void ath_bstuck_process(struct ath_softc *sc)
{
	DPRINTF(sc, ATH_DEBUG_BEACON,
		"%s: stuck beacon; resetting (bmiss count %u)\n",
		__func__, sc->sc_bmisscount);
	ath_internal_reset(sc);
}

/*
 * Configure the beacon and sleep timers.
 *
 * When operating as an AP this resets the TSF and sets
 * up the hardware to notify us when we need to issue beacons.
 *
 * When operating in station mode this sets up the beacon
 * timers according to the timestamp of the last received
 * beacon and the current TSF, configures PCF and DTIM
 * handling, programs the sleep registers so the hardware
 * will wakeup in time to receive beacons, and configures
 * the beacon miss handling so we'll receive a BMISS
 * interrupt when we stop seeing beacons from the AP
 * we've associated with.
 */

void ath_beacon_config(struct ath_softc *sc, int if_id)
{
#define TSF_TO_TU(_h,_l)					\
	((((u_int32_t)(_h)) << 22) | (((u_int32_t)(_l)) >> 10))
	struct ath_hal *ah = sc->sc_ah;
	u_int32_t nexttbtt, intval;
	struct ath_beacon_config conf;
	enum hal_opmode av_opmode;

	if (if_id != ATH_IF_ID_ANY)
		av_opmode = sc->sc_vaps[if_id]->av_opmode;
	else
		av_opmode = sc->sc_opmode;

	memzero(&conf, sizeof(struct ath_beacon_config));

	/* FIXME: Use default values for now - Sujith */
	/* Query beacon configuration first */
	/*
	 * Protocol stack doesn't support dynamic beacon configuration,
	 * use default configurations.
	 */
	conf.beacon_interval = ATH_DEFAULT_BINTVAL;
	conf.listen_interval = 1;
	conf.dtim_period = conf.beacon_interval;
	conf.dtim_count = 1;
	conf.bmiss_timeout = ATH_DEFAULT_BMISS_LIMIT * conf.beacon_interval;

	/* extract tstamp from last beacon and convert to TU */
	nexttbtt = TSF_TO_TU(LE_READ_4(conf.u.last_tstamp + 4),
			     LE_READ_4(conf.u.last_tstamp));
	/* XXX conditionalize multi-bss support? */
	if (sc->sc_opmode == HAL_M_HOSTAP) {
		/*
		 * For multi-bss ap support beacons are either staggered
		 * evenly over N slots or burst together.  For the former
		 * arrange for the SWBA to be delivered for each slot.
		 * Slots that are not occupied will generate nothing.
		 */
		/* NB: the beacon interval is kept internally in TU's */
		intval = conf.beacon_interval & HAL_BEACON_PERIOD;
		if (sc->sc_stagbeacons)
			intval /= ATH_BCBUF;    /* for staggered beacons */
		if ((sc->sc_nostabeacons) &&
		    (av_opmode == HAL_M_HOSTAP))
			nexttbtt = 0;
	} else {
		intval = conf.beacon_interval & HAL_BEACON_PERIOD;
	}

	if (nexttbtt == 0)      /* e.g. for ap mode */
		nexttbtt = intval;
	else if (intval)        /* NB: can be 0 for monitor mode */
		nexttbtt = roundup(nexttbtt, intval);
	DPRINTF(sc, ATH_DEBUG_BEACON, "%s: nexttbtt %u intval %u (%u)\n",
		__func__, nexttbtt, intval, conf.beacon_interval);
	/* Check for HAL_M_HOSTAP and sc_nostabeacons for WDS client */
	if ((sc->sc_opmode == HAL_M_STA) ||
	     ((sc->sc_opmode == HAL_M_HOSTAP) &&
	      (av_opmode == HAL_M_STA) &&
	      (sc->sc_nostabeacons))) {
		struct hal_beacon_state bs;
		u_int64_t tsf;
		u_int32_t tsftu;
		int dtimperiod, dtimcount, sleepduration;
		int cfpperiod, cfpcount;

		/*
		 * Setup dtim and cfp parameters according to
		 * last beacon we received (which may be none).
		 */
		dtimperiod = conf.dtim_period;
		if (dtimperiod <= 0)        /* NB: 0 if not known */
			dtimperiod = 1;
		dtimcount = conf.dtim_count;
		if (dtimcount >= dtimperiod)    /* NB: sanity check */
			dtimcount = 0;      /* XXX? */
		cfpperiod = 1;          /* NB: no PCF support yet */
		cfpcount = 0;

		sleepduration = conf.listen_interval * intval;
		if (sleepduration <= 0)
			sleepduration = intval;

#define FUDGE   2
		/*
		 * Pull nexttbtt forward to reflect the current
		 * TSF and calculate dtim+cfp state for the result.
		 */
		tsf = ath9k_hw_gettsf64(ah);
		tsftu = TSF_TO_TU(tsf>>32, tsf) + FUDGE;
		do {
			nexttbtt += intval;
			if (--dtimcount < 0) {
				dtimcount = dtimperiod - 1;
				if (--cfpcount < 0)
					cfpcount = cfpperiod - 1;
			}
		} while (nexttbtt < tsftu);
#undef FUDGE
		memzero(&bs, sizeof(bs));
		bs.bs_intval = intval;
		bs.bs_nexttbtt = nexttbtt;
		bs.bs_dtimperiod = dtimperiod*intval;
		bs.bs_nextdtim = bs.bs_nexttbtt + dtimcount*intval;
		bs.bs_cfpperiod = cfpperiod*bs.bs_dtimperiod;
		bs.bs_cfpnext = bs.bs_nextdtim + cfpcount*bs.bs_dtimperiod;
		bs.bs_cfpmaxduration = 0;
		/*
		 * Calculate the number of consecutive beacons to miss
		 * before taking a BMISS interrupt.  The configuration
		 * is specified in TU so we only need calculate based
		 * on the beacon interval.  Note that we clamp the
		 * result to at most 15 beacons.
		 */
		if (sleepduration > intval) {
			bs.bs_bmissthreshold =
				conf.listen_interval *
					ATH_DEFAULT_BMISS_LIMIT / 2;
		} else {
			bs.bs_bmissthreshold =
				howmany(conf.bmiss_timeout, intval);
			if (bs.bs_bmissthreshold > 15)
				bs.bs_bmissthreshold = 15;
			else if (bs.bs_bmissthreshold <= 0)
				bs.bs_bmissthreshold = 1;
		}

		/*
		 * Calculate sleep duration.  The configuration is
		 * given in ms.  We insure a multiple of the beacon
		 * period is used.  Also, if the sleep duration is
		 * greater than the DTIM period then it makes senses
		 * to make it a multiple of that.
		 *
		 * XXX fixed at 100ms
		 */

		bs.bs_sleepduration =
			roundup(IEEE80211_MS_TO_TU(100), sleepduration);
		if (bs.bs_sleepduration > bs.bs_dtimperiod)
			bs.bs_sleepduration = bs.bs_dtimperiod;

		DPRINTF(sc, ATH_DEBUG_BEACON,
			"%s: tsf %llu "
			"tsf:tu %u "
			"intval %u "
			"nexttbtt %u "
			"dtim %u "
			"nextdtim %u "
			"bmiss %u "
			"sleep %u "
			"cfp:period %u "
			"maxdur %u "
			"next %u "
			"timoffset %u\n"
			, __func__
			, (unsigned long long)tsf, tsftu
			, bs.bs_intval
			, bs.bs_nexttbtt
			, bs.bs_dtimperiod
			, bs.bs_nextdtim
			, bs.bs_bmissthreshold
			, bs.bs_sleepduration
			, bs.bs_cfpperiod
			, bs.bs_cfpmaxduration
			, bs.bs_cfpnext
			, bs.bs_timoffset
			);

		if (!(sc->sc_nostabeacons)) {
			ath9k_hw_set_interrupts(ah, 0);
			ath9k_hw_set_sta_beacon_timers(ah, &bs);
			sc->sc_imask |= HAL_INT_BMISS;
			ath9k_hw_set_interrupts(ah, sc->sc_imask);
		}
	} else {
		u_int64_t tsf;
		u_int32_t tsftu;
		ath9k_hw_set_interrupts(ah, 0);
		if (nexttbtt == intval)
			intval |= HAL_BEACON_RESET_TSF;
		if (sc->sc_opmode == HAL_M_IBSS) {
			/*
			 * Pull nexttbtt forward to reflect the current
			 * TSF .
			 */
#define FUDGE   2
			if (!(intval & HAL_BEACON_RESET_TSF)) {
				tsf = ath9k_hw_gettsf64(ah);
				tsftu = TSF_TO_TU((u_int32_t)(tsf>>32),
					(u_int32_t)tsf) + FUDGE;
				do {
					nexttbtt += intval;
				} while (nexttbtt < tsftu);
			}
#undef FUDGE
			DPRINTF(sc, ATH_DEBUG_BEACON,
				"%s: IBSS nexttbtt %u intval %u (%u)\n",
				__func__, nexttbtt,
				intval & ~HAL_BEACON_RESET_TSF,
				conf.beacon_interval);

			/*
			 * In IBSS mode enable the beacon timers but only
			 * enable SWBA interrupts if we need to manually
			 * prepare beacon frames.  Otherwise we use a
			 * self-linked tx descriptor and let the hardware
			 * deal with things.
			 */
			intval |= HAL_BEACON_ENA;
			if (!sc->sc_hasveol)
				sc->sc_imask |= HAL_INT_SWBA;
			ath_beaconq_config(sc);
		} else if (sc->sc_opmode == HAL_M_HOSTAP) {
			/*
			 * In AP mode we enable the beacon timers and
			 * SWBA interrupts to prepare beacon frames.
			 */
			intval |= HAL_BEACON_ENA;
			sc->sc_imask |= HAL_INT_SWBA;   /* beacon prepare */
			ath_beaconq_config(sc);
		}
		ath9k_hw_beaconinit(ah, nexttbtt, intval);
		sc->sc_bmisscount = 0;
		ath9k_hw_set_interrupts(ah, sc->sc_imask);
		/*
		 * When using a self-linked beacon descriptor in
		 * ibss mode load it once here.
		 */
		if (sc->sc_opmode == HAL_M_IBSS && sc->sc_hasveol)
			ath_beacon_start_adhoc(sc, 0);
	}
#undef TSF_TO_TU
}

/* Function to collect beacon rssi data and resync beacon if necessary */

void ath_beacon_sync(struct ath_softc *sc, int if_id)
{
	/*
	 * Resync beacon timers using the tsf of the
	 * beacon frame we just received.
	 */
	ath_beacon_config(sc, if_id);
	sc->sc_beacons = 1;
}
