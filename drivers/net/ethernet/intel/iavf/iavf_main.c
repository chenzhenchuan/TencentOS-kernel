// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2013 - 2019 Intel Corporation. */

#include "iavf.h"
#include "iavf_helper.h"
#include "iavf_prototype.h"
#include "iavf_client.h"
/* All iavf tracepoints are defined by the include below, which must
 * be included exactly once across the whole kernel with
 * CREATE_TRACE_POINTS defined
 */
#define CREATE_TRACE_POINTS
#include "iavf_trace.h"

static int iavf_setup_all_tx_resources(struct iavf_adapter *adapter);
static int iavf_setup_all_rx_resources(struct iavf_adapter *adapter);
static int iavf_close(struct net_device *netdev);
static void iavf_init_get_resources(struct iavf_adapter *adapter);
static int iavf_check_reset_complete(struct iavf_hw *hw);
static void iavf_handle_reset(struct iavf_adapter *adapter);

char iavf_driver_name[] = "iavf";
static const char iavf_driver_string[] =
	"Intel(R) Ethernet Adaptive Virtual Function Network Driver";

#ifndef DRV_VERSION_LOCAL
#define DRV_VERSION_LOCAL
#endif /* DRV_VERSION_LOCAL */

#define DRV_VF_VERSION_DESC "_rc12"

#define DRV_VERSION_MAJOR 3
#define DRV_VERSION_MINOR 7
#define DRV_VERSION_BUILD 32
#define DRV_VERSION __stringify(DRV_VERSION_MAJOR) "." \
	     __stringify(DRV_VERSION_MINOR) "." \
	     __stringify(DRV_VERSION_BUILD) \
	     DRV_VF_VERSION_DESC __stringify(DRV_VERSION_LOCAL)
const char iavf_driver_version[] = DRV_VERSION;
static const char iavf_copyright[] =
	"Copyright(c) 2013 - 2019 Intel Corporation.";

/* iavf_pci_tbl - PCI Device ID Table
 *
 * Wildcard entries (PCI_ANY_ID) should come last
 * Last entry must be all 0s
 *
 * { Vendor ID, Device ID, SubVendor ID, SubDevice ID,
 *   Class, Class Mask, private data (not used) }
 */
static const struct pci_device_id iavf_pci_tbl[] = {
	{PCI_VDEVICE(INTEL, IAVF_DEV_ID_VF), 0},
	{PCI_VDEVICE(INTEL, IAVF_DEV_ID_VF_HV), 0},
	{PCI_VDEVICE(INTEL, IAVF_DEV_ID_X722_VF), 0},
	{PCI_VDEVICE(INTEL, IAVF_DEV_ID_ADAPTIVE_VF), 0},
	/* required last entry */
	{0, }
};

MODULE_DEVICE_TABLE(pci, iavf_pci_tbl);

MODULE_ALIAS("i40evf");
MODULE_AUTHOR("Intel Corporation, <linux.nics@intel.com>");
MODULE_DESCRIPTION("Intel(R) Ethernet Adaptive Virtual Function Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

static const struct net_device_ops iavf_netdev_ops;
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
static const struct net_device_ops_ext iavf_netdev_ops_ext;
#endif /* HAVE_RHEL6_NET_DEVICE_OPS_EXT */
struct workqueue_struct *iavf_wq;

/**
 * iavf_schedule_reset - Set the flags and schedule a reset event
 * @adapter: board private structure
 **/
void iavf_schedule_reset(struct iavf_adapter *adapter)
{
	adapter->flags |= IAVF_FLAG_RESET_NEEDED;
	mod_delayed_work(iavf_wq, &adapter->watchdog_task, 0);
}

/**
 * iavf_tx_timeout - Respond to a Tx Hang
 * @netdev: network interface device structure
 **/
static void iavf_tx_timeout(struct net_device *netdev)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	adapter->tx_timeout_count++;
	iavf_schedule_reset(adapter);
}

/**
 * iavf_misc_irq_disable - Mask off interrupt generation on the NIC
 * @adapter: board private structure
 **/
static void iavf_misc_irq_disable(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;

	if (!adapter->msix_entries)
		return;

	wr32(hw, IAVF_VFINT_DYN_CTL01, 0);

	iavf_flush(hw);

	synchronize_irq(adapter->msix_entries[0].vector);
}

/**
 * iavf_misc_irq_enable - Enable default interrupt generation settings
 * @adapter: board private structure
 **/
static void iavf_misc_irq_enable(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;

	wr32(hw, IAVF_VFINT_DYN_CTL01, IAVF_VFINT_DYN_CTL01_INTENA_MASK |
				       IAVF_VFINT_DYN_CTL01_ITR_INDX_MASK);
	wr32(hw, IAVF_VFINT_ICR0_ENA1, IAVF_VFINT_ICR0_ENA1_ADMINQ_MASK);

	iavf_flush(hw);
}

/**
 * iavf_irq_disable - Mask off interrupt generation on the NIC
 * @adapter: board private structure
 **/
static void iavf_irq_disable(struct iavf_adapter *adapter)
{
	int i;
	struct iavf_hw *hw = &adapter->hw;

	if (!adapter->msix_entries)
		return;

	for (i = 1; i < adapter->num_msix_vectors; i++) {
		wr32(hw, IAVF_VFINT_DYN_CTLN1(i - 1), 0);
		synchronize_irq(adapter->msix_entries[i].vector);
	}
	iavf_flush(hw);
}

/**
 * iavf_irq_enable_queues - Enable interrupt for specified queues
 * @adapter: board private structure
 * @mask: bitmap of queues to enable
 **/
void iavf_irq_enable_queues(struct iavf_adapter *adapter, u32 mask)
{
	struct iavf_hw *hw = &adapter->hw;
	int i;

	for (i = 1; i < adapter->num_msix_vectors; i++) {
		if (mask & BIT(i - 1)) {
			wr32(hw, IAVF_VFINT_DYN_CTLN1(i - 1),
			     IAVF_VFINT_DYN_CTLN1_INTENA_MASK |
			     IAVF_VFINT_DYN_CTLN1_ITR_INDX_MASK);
		}
	}
}

/**
 * iavf_irq_enable - Enable default interrupt generation settings
 * @adapter: board private structure
 * @flush: boolean value whether to run rd32()
 **/
void iavf_irq_enable(struct iavf_adapter *adapter, bool flush)
{
	struct iavf_hw *hw = &adapter->hw;

	iavf_misc_irq_enable(adapter);
	iavf_irq_enable_queues(adapter, ~0);

	if (flush)
		iavf_flush(hw);
}

/**
 * iavf_msix_aq - Interrupt handler for vector 0
 * @irq: interrupt number
 * @data: pointer to netdev
 **/
static irqreturn_t iavf_msix_aq(int irq, void *data)
{
	struct net_device *netdev = data;
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct iavf_hw *hw = &adapter->hw;

	/* handle non-queue interrupts */
	rd32(hw, IAVF_VFINT_ICR01);
	rd32(hw, IAVF_VFINT_ICR0_ENA1);

	/* schedule work on the private workqueue */
	queue_work(iavf_wq, &adapter->adminq_task);

	return IRQ_HANDLED;
}

/**
 * iavf_msix_clean_rings - MSIX mode Interrupt Handler
 * @irq: interrupt number
 * @data: pointer to a q_vector
 **/
static irqreturn_t iavf_msix_clean_rings(int irq, void *data)
{
	struct iavf_q_vector *q_vector = data;

	if (!q_vector->tx.ring && !q_vector->rx.ring)
		return IRQ_HANDLED;

	napi_schedule_irqoff(&q_vector->napi);

	return IRQ_HANDLED;
}

/**
 * iavf_map_vector_to_rxq - associate irqs with rx queues
 * @adapter: board private structure
 * @v_idx: interrupt number
 * @r_idx: queue number
 **/
static void
iavf_map_vector_to_rxq(struct iavf_adapter *adapter, int v_idx, int r_idx)
{
	struct iavf_q_vector *q_vector = &adapter->q_vectors[v_idx];
	struct iavf_ring *rx_ring = &adapter->rx_rings[r_idx];
	struct iavf_hw *hw = &adapter->hw;

	rx_ring->q_vector = q_vector;
	rx_ring->next = q_vector->rx.ring;
	rx_ring->vsi = &adapter->vsi;
	q_vector->rx.ring = rx_ring;
	q_vector->rx.count++;
	q_vector->rx.next_update = jiffies + 1;
	q_vector->rx.target_itr = ITR_TO_REG(rx_ring->itr_setting);
	q_vector->ring_mask |= BIT(r_idx);
	wr32(hw, IAVF_VFINT_ITRN1(IAVF_RX_ITR, q_vector->reg_idx),
	     q_vector->rx.current_itr);
	q_vector->rx.current_itr = q_vector->rx.target_itr;
}

/**
 * iavf_map_vector_to_txq - associate irqs with tx queues
 * @adapter: board private structure
 * @v_idx: interrupt number
 * @t_idx: queue number
 **/
static void
iavf_map_vector_to_txq(struct iavf_adapter *adapter, int v_idx, int t_idx)
{
	struct iavf_q_vector *q_vector = &adapter->q_vectors[v_idx];
	struct iavf_ring *tx_ring = &adapter->tx_rings[t_idx];
	struct iavf_hw *hw = &adapter->hw;

	tx_ring->q_vector = q_vector;
	tx_ring->next = q_vector->tx.ring;
	tx_ring->vsi = &adapter->vsi;
	q_vector->tx.ring = tx_ring;
	q_vector->tx.count++;
	q_vector->tx.next_update = jiffies + 1;
	q_vector->tx.target_itr = ITR_TO_REG(tx_ring->itr_setting);
	q_vector->num_ringpairs++;
	wr32(hw, IAVF_VFINT_ITRN1(IAVF_TX_ITR, q_vector->reg_idx),
	     q_vector->tx.target_itr);
	q_vector->tx.current_itr = q_vector->tx.target_itr;
}

/**
 * iavf_map_rings_to_vectors - Maps descriptor rings to vectors
 * @adapter: board private structure to initialize
 *
 * This function maps descriptor rings to the queue-specific vectors
 * we were allotted through the MSI-X enabling code.  Ideally, we'd have
 * one vector per ring/queue, but on a constrained vector budget, we
 * group the rings as "efficiently" as possible.  You would add new
 * mapping configurations in here.
 **/
static void iavf_map_rings_to_vectors(struct iavf_adapter *adapter)
{
	int rings_remaining = adapter->num_active_queues;
	int ridx = 0, vidx = 0;
	int q_vectors;

	q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (; ridx < rings_remaining; ridx++) {
		iavf_map_vector_to_rxq(adapter, vidx, ridx);
		iavf_map_vector_to_txq(adapter, vidx, ridx);

		/* In the case where we have more queues than vectors, continue
		 * round-robin on vectors until all queues are mapped.
		 */
		if (++vidx >= q_vectors)
			vidx = 0;
	}

	adapter->aq_required |= IAVF_FLAG_AQ_MAP_VECTORS;
}

#ifdef HAVE_IRQ_AFFINITY_NOTIFY
/**
 * iavf_irq_affinity_notify - Callback for affinity changes
 * @notify: context as to what irq was changed
 * @mask: the new affinity mask
 *
 * This is a callback function used by the irq_set_affinity_notifier function
 * so that we may register to receive changes to the irq affinity masks.
 **/
static void iavf_irq_affinity_notify(struct irq_affinity_notify *notify,
				     const cpumask_t *mask)
{
	struct iavf_q_vector *q_vector =
		container_of(notify, struct iavf_q_vector, affinity_notify);

	cpumask_copy(&q_vector->affinity_mask, mask);
}

/**
 * iavf_irq_affinity_release - Callback for affinity notifier release
 * @ref: internal core kernel usage
 *
 * This is a callback function used by the irq_set_affinity_notifier function
 * to inform the current notification subscriber that they will no longer
 * receive notifications.
 **/
static void iavf_irq_affinity_release(struct kref *ref) {}
#endif /* HAVE_IRQ_AFFINITY_NOTIFY */

/**
 * iavf_request_traffic_irqs - Initialize MSI-X interrupts
 * @adapter: board private structure
 * @basename: device basename
 *
 * Allocates MSI-X vectors for tx and rx handling, and requests
 * interrupts from the kernel.
 **/
static int
iavf_request_traffic_irqs(struct iavf_adapter *adapter, char *basename)
{
	unsigned int vector, q_vectors;
	unsigned int rx_int_idx = 0, tx_int_idx = 0;
	int irq_num, err;
	int cpu;

	iavf_irq_disable(adapter);
	/* Decrement for Other and TCP Timer vectors */
	q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (vector = 0; vector < q_vectors; vector++) {
		struct iavf_q_vector *q_vector = &adapter->q_vectors[vector];
		irq_num = adapter->msix_entries[vector + NONQ_VECS].vector;

		if (q_vector->tx.ring && q_vector->rx.ring) {
			snprintf(q_vector->name, sizeof(q_vector->name),
				 "iavf-%s-TxRx-%d", basename, rx_int_idx++);
			tx_int_idx++;
		} else if (q_vector->rx.ring) {
			snprintf(q_vector->name, sizeof(q_vector->name),
				 "iavf-%s-rx-%d", basename, rx_int_idx++);
		} else if (q_vector->tx.ring) {
			snprintf(q_vector->name, sizeof(q_vector->name),
				 "iavf-%s-tx-%d", basename, tx_int_idx++);
		} else {
			/* skip this unused q_vector */
			continue;
		}
		err = request_irq(irq_num,
				  iavf_msix_clean_rings,
				  0,
				  q_vector->name,
				  q_vector);
		if (err) {
			dev_info(&adapter->pdev->dev,
				 "Request_irq failed, error: %d\n", err);
			goto free_queue_irqs;
		}
#ifdef HAVE_IRQ_AFFINITY_NOTIFY
		/* register for affinity change notifications */
		q_vector->affinity_notify.notify = iavf_irq_affinity_notify;
		q_vector->affinity_notify.release =
						   iavf_irq_affinity_release;
		irq_set_affinity_notifier(irq_num, &q_vector->affinity_notify);
#endif
#ifdef HAVE_IRQ_AFFINITY_HINT
		/* Spread the IRQ affinity hints across online CPUs. Note that
		 * get_cpu_mask returns a mask with a permanent lifetime so
		 * it's safe to use as a hint for irq_set_affinity_hint.
		 */
		cpu = cpumask_local_spread(q_vector->v_idx, -1);
		irq_set_affinity_hint(irq_num, get_cpu_mask(cpu));
#endif /* HAVE_IRQ_AFFINITY_HINT */
	}

	return 0;

free_queue_irqs:
	while (vector) {
		vector--;
		irq_num = adapter->msix_entries[vector + NONQ_VECS].vector;
#ifdef HAVE_IRQ_AFFINITY_NOTIFY
		irq_set_affinity_notifier(irq_num, NULL);
#endif
#ifdef HAVE_IRQ_AFFINITY_HINT
		irq_set_affinity_hint(irq_num, NULL);
#endif
		free_irq(irq_num, &adapter->q_vectors[vector]);
	}
	return err;
}

/**
 * iavf_request_misc_irq - Initialize MSI-X interrupts
 * @adapter: board private structure
 *
 * Allocates MSI-X vector 0 and requests interrupts from the kernel. This
 * vector is only for the admin queue, and stays active even when the netdev
 * is closed.
 **/
static int iavf_request_misc_irq(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int err;

	snprintf(adapter->misc_vector_name,
		 sizeof(adapter->misc_vector_name) - 1, "iavf-%s:mbx",
		 dev_name(&adapter->pdev->dev));
	err = request_irq(adapter->msix_entries[0].vector,
			  &iavf_msix_aq, 0,
			  adapter->misc_vector_name, netdev);
	if (err) {
		dev_err(&adapter->pdev->dev,
			"request_irq for %s failed: %d\n",
			adapter->misc_vector_name, err);
		free_irq(adapter->msix_entries[0].vector, netdev);
	}
	return err;
}

/**
 * iavf_free_traffic_irqs - Free MSI-X interrupts
 * @adapter: board private structure
 *
 * Frees all MSI-X vectors other than 0.
 **/
static void iavf_free_traffic_irqs(struct iavf_adapter *adapter)
{
	int vector, irq_num, q_vectors;

	if (!adapter->msix_entries)
		return;

	q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (vector = 0; vector < q_vectors; vector++) {
		irq_num = adapter->msix_entries[vector + NONQ_VECS].vector;
#ifdef HAVE_IRQ_AFFINITY_NOTIFY
		irq_set_affinity_notifier(irq_num, NULL);
#endif
#ifdef HAVE_IRQ_AFFINITY_HINT
		irq_set_affinity_hint(irq_num, NULL);
#endif
		free_irq(irq_num, &adapter->q_vectors[vector]);
	}
}

/**
 * iavf_free_misc_irq - Free MSI-X miscellaneous vector
 * @adapter: board private structure
 *
 * Frees MSI-X vector 0.
 **/
static void iavf_free_misc_irq(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;

	if (!adapter->msix_entries)
		return;

	free_irq(adapter->msix_entries[0].vector, netdev);
}

/**
 * iavf_configure_tx - Configure Transmit Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Tx unit of the MAC after a reset.
 **/
static void iavf_configure_tx(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;
	int i;

	for (i = 0; i < adapter->num_active_queues; i++)
		adapter->tx_rings[i].tail = hw->hw_addr + IAVF_QTX_TAIL1(i);
}

/**
 * iavf_configure_rx - Configure Receive Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Rx unit of the MAC after a reset.
 **/
static void iavf_configure_rx(struct iavf_adapter *adapter)
{
	unsigned int rx_buf_len = IAVF_RXBUFFER_2048;
	struct iavf_hw *hw = &adapter->hw;
	int i;

#ifdef CONFIG_IAVF_DISABLE_PACKET_SPLIT
	/* reset Rx buffer length for an Ethernet packet */
	rx_buf_len = IAVF_RXBUFFER_1536 - NET_IP_ALIGN;
	if ((adapter->netdev->mtu + IAVF_PACKET_HDR_PAD) > rx_buf_len)
		rx_buf_len = adapter->netdev->mtu + IAVF_PACKET_HDR_PAD;
#else
	/* Legacy Rx will always default to a 2048 buffer size. */
#if (PAGE_SIZE < 8192)
	if (!(adapter->flags & IAVF_FLAG_LEGACY_RX)) {
		struct net_device *netdev = adapter->netdev;

		/* For jumbo frames on systems with 4K pages we have to use
		 * an order 1 page, so we might as well increase the size
		 * of our Rx buffer to make better use of the available space
		 */
		rx_buf_len = IAVF_RXBUFFER_3072;

		/* We use a 1536 buffer size for configurations with
		 * standard Ethernet mtu.  On x86 this gives us enough room
		 * for shared info and 192 bytes of padding.
		 */
		if (!IAVF_2K_TOO_SMALL_WITH_PADDING &&
		    (netdev->mtu <= ETH_DATA_LEN))
			rx_buf_len = IAVF_RXBUFFER_1536 - NET_IP_ALIGN;
	}
#endif
#endif /* CONFIG_IAVF_DISABLE_PACKET_SPLIT */

	for (i = 0; i < adapter->num_active_queues; i++) {
		adapter->rx_rings[i].tail = hw->hw_addr + IAVF_QRX_TAIL1(i);
		adapter->rx_rings[i].rx_buf_len = rx_buf_len;

		if (adapter->flags & IAVF_FLAG_LEGACY_RX)
			clear_ring_build_skb_enabled(&adapter->rx_rings[i]);
		else
			set_ring_build_skb_enabled(&adapter->rx_rings[i]);
	}
}

#ifdef HAVE_VLAN_RX_REGISTER
/**
 * iavf_vlan_rx_register - Register for RX vlan filtering, enable VLAN
 * tag stripping
 * @netdev: netdevice structure
 * @grp: vlan group data
 **/
static void iavf_vlan_rx_register(struct net_device *netdev,
				    struct vlan_group *grp)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	/* Since VLAN tag stripping is always enabled, just store vlgrp. */
	adapter->vsi.vlgrp = grp;

}

#endif
/**
 * iavf_find_vlan - Search filter list for specific vlan filter
 * @adapter: board private structure
 * @vlan: vlan tag
 *
 * Returns ptr to the filter object or NULL. Must be called while holding the
 * mac_vlan_list_lock.
 **/
static struct
iavf_vlan_filter *iavf_find_vlan(struct iavf_adapter *adapter, u16 vlan)
{
	struct iavf_vlan_filter *f;

	list_for_each_entry(f, &adapter->vlan_filter_list, list) {
		if (vlan == f->vlan)
			return f;
	}
	return NULL;
}

/**
 * iavf_add_vlan - Add a vlan filter to the list
 * @adapter: board private structure
 * @vlan: VLAN tag
 *
 * Returns ptr to the filter object or NULL when no memory available.
 **/
static struct
iavf_vlan_filter *iavf_add_vlan(struct iavf_adapter *adapter, u16 vlan)
{
	struct iavf_vlan_filter *f = NULL;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	f = iavf_find_vlan(adapter, vlan);
	if (!f) {
		f = kzalloc(sizeof(*f), GFP_ATOMIC);
		if (!f)
			goto clearout;

		f->vlan = vlan;

		list_add_tail(&f->list, &adapter->vlan_filter_list);
		f->add = true;
		adapter->aq_required |= IAVF_FLAG_AQ_ADD_VLAN_FILTER;
	}

clearout:
	spin_unlock_bh(&adapter->mac_vlan_list_lock);
	return f;
}

/**
 * iavf_del_vlan - Remove a vlan filter from the list
 * @adapter: board private structure
 * @vlan: VLAN tag
 **/
static void iavf_del_vlan(struct iavf_adapter *adapter, u16 vlan)
{
	struct iavf_vlan_filter *f;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	f = iavf_find_vlan(adapter, vlan);
	if (f) {
		f->remove = true;
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_VLAN_FILTER;
	}

	spin_unlock_bh(&adapter->mac_vlan_list_lock);
}

/**
 * iavf_restore_filters
 * @adapter: board private structure
 *
 * Restore existing non MAC filters when VF netdev comes back up
 **/
static void iavf_restore_filters(struct iavf_adapter *adapter)
{
	/* re-add all VLAN filters */
#ifdef HAVE_VLAN_RX_REGISTER
	if (adapter->vsi.vlgrp)
		iavf_vlan_rx_register(adapter->netdev, adapter->vsi.vlgrp);
#else /* HAVE_VLAN_RX_REGISTER */
	if (VLAN_ALLOWED(adapter)) {
		u16 vid;

		for_each_set_bit(vid, adapter->vsi.active_vlans, VLAN_N_VID)
			iavf_add_vlan(adapter, vid);
	}

#endif /* HAVE_VLAN_RX_REGISTER */
}

/**
 * iavf_vlan_rx_add_vid - Add a VLAN filter to a device
 * @netdev: network device struct
 * @proto: unused protocol data
 * @vid: VLAN tag
 **/
#ifdef HAVE_INT_NDO_VLAN_RX_ADD_VID
#ifdef NETIF_F_HW_VLAN_CTAG_RX
static int iavf_vlan_rx_add_vid(struct net_device *netdev,
				__always_unused __be16 proto, u16 vid)
#else
static int iavf_vlan_rx_add_vid(struct net_device *netdev, u16 vid)
#endif
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	if (!VLAN_ALLOWED(adapter))
		return -EIO;

	if (iavf_add_vlan(adapter, vid) == NULL)
		return -ENOMEM;

#ifndef HAVE_VLAN_RX_REGISTER
	set_bit(vid, adapter->vsi.active_vlans);
#endif /* HAVE_VLAN_RX_REGISTER */
	return 0;
}
#else
static void iavf_vlan_rx_add_vid(struct net_device *netdev, u16 vid)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	if (!VLAN_ALLOWED(adapter))
		return;

	iavf_add_vlan(adapter, vid);
#ifndef HAVE_VLAN_RX_REGISTER
	set_bit(vid, adapter->vsi.active_vlans);
#endif /* HAVE_VLAN_RX_REGISTER */
}
#endif

/**
 * iavf_vlan_rx_kill_vid - Remove a VLAN filter from a device
 * @netdev: network device struct
 * @proto: unused protocol data
 * @vid: VLAN tag
 **/
#ifdef HAVE_INT_NDO_VLAN_RX_ADD_VID
#ifdef NETIF_F_HW_VLAN_CTAG_RX
static int iavf_vlan_rx_kill_vid(struct net_device *netdev,
				 __always_unused __be16 proto, u16 vid)
#else
static int iavf_vlan_rx_kill_vid(struct net_device *netdev, u16 vid)
#endif
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	if (!VLAN_ALLOWED(adapter))
		return -EIO;

	iavf_del_vlan(adapter, vid);
#ifndef HAVE_VLAN_RX_REGISTER
	clear_bit(vid, adapter->vsi.active_vlans);
#endif /* HAVE_VLAN_RX_REGISTER */

	return 0;
}
#else
static void iavf_vlan_rx_kill_vid(struct net_device *netdev, u16 vid)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	if (!VLAN_ALLOWED(adapter))
		return;

	iavf_del_vlan(adapter, vid);
#ifndef HAVE_VLAN_RX_REGISTER
	clear_bit(vid, adapter->vsi.active_vlans);
#endif /* HAVE_VLAN_RX_REGISTER */
}
#endif

/**
 * iavf_find_filter - Search filter list for specific mac filter
 * @adapter: board private structure
 * @macaddr: the MAC address
 *
 * Returns ptr to the filter object or NULL. Must be called while holding the
 * mac_vlan_list_lock.
 **/
static struct
iavf_mac_filter *iavf_find_filter(struct iavf_adapter *adapter,
				  const u8 *macaddr)
{
	struct iavf_mac_filter *f;

	if (!macaddr)
		return NULL;

	list_for_each_entry(f, &adapter->mac_filter_list, list) {
		if (ether_addr_equal(macaddr, f->macaddr))
			return f;
	}
	return NULL;
}

/**
 * iavf_add_filter - Add a mac filter to the filter list
 * @adapter: board private structure
 * @macaddr: the MAC address
 *
 * Returns ptr to the filter object or NULL when no memory available.
 **/
static struct
iavf_mac_filter *iavf_add_filter(struct iavf_adapter *adapter,
				 const u8 *macaddr)
{
	struct iavf_mac_filter *f;

	if (!macaddr)
		return NULL;

	f = iavf_find_filter(adapter, macaddr);
	if (!f) {
		f = kzalloc(sizeof(*f), GFP_ATOMIC);
		if (!f)
			return f;

		ether_addr_copy(f->macaddr, macaddr);

		list_add_tail(&f->list, &adapter->mac_filter_list);
		f->add = true;
		adapter->aq_required |= IAVF_FLAG_AQ_ADD_MAC_FILTER;
	} else {
		f->remove = false;
	}

	return f;
}

/**
 * iavf_set_mac - NDO callback to set port mac address
 * @netdev: network interface device structure
 * @p: pointer to an address structure
 *
 * Returns 0 on success, negative on failure
 **/
static int iavf_set_mac(struct net_device *netdev, void *p)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct iavf_hw *hw = &adapter->hw;
	struct iavf_mac_filter *f;
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	if (ether_addr_equal(netdev->dev_addr, addr->sa_data))
		return 0;

	if (adapter->flags & IAVF_FLAG_ADDR_SET_BY_PF)
		return -EPERM;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	f = iavf_find_filter(adapter, hw->mac.addr);
	if (f) {
		f->remove = true;
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_MAC_FILTER;
	}

	f = iavf_add_filter(adapter, addr->sa_data);

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	if (f) {
		ether_addr_copy(hw->mac.addr, addr->sa_data);
		ether_addr_copy(netdev->dev_addr, adapter->hw.mac.addr);
	}

	return (f == NULL) ? -ENOMEM : 0;
}

/**
 * iavf_addr_sync - Callback for dev_(mc|uc)_sync to add address
 * @netdev: the netdevice
 * @addr: address to add
 *
 * Called by __dev_(mc|uc)_sync when an address needs to be added. We call
 * __dev_(uc|mc)_sync from .set_rx_mode and guarantee to hold the hash lock.
 */
static int iavf_addr_sync(struct net_device *netdev, const u8 *addr)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	if (iavf_add_filter(adapter, addr))
		return 0;
	else
		return -ENOMEM;
}

/**
 * iavf_addr_unsync - Callback for dev_(mc|uc)_sync to remove address
 * @netdev: the netdevice
 * @addr: address to add
 *
 * Called by __dev_(mc|uc)_sync when an address needs to be removed. We call
 * __dev_(uc|mc)_sync from .set_rx_mode and guarantee to hold the hash lock.
 */
static int iavf_addr_unsync(struct net_device *netdev, const u8 *addr)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct iavf_mac_filter *f;

	/* Under some circumstances, we might receive a request to delete
	 * our own device address from our uc list. Because we store the
	 * device address in the VSI's MAC/VLAN filter list, we need to ignore
	 * such requests and not delete our device address from this list.
	 */
	if (ether_addr_equal(addr, netdev->dev_addr))
		return 0;

	f = iavf_find_filter(adapter, addr);
	if (f) {
		f->remove = true;
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_MAC_FILTER;
	}

	return 0;
}

/**
 * iavf_set_rx_mode - NDO callback to set the netdev filters
 * @netdev: network interface device structure
 **/
static void iavf_set_rx_mode(struct net_device *netdev)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	__dev_uc_sync(netdev, iavf_addr_sync, iavf_addr_unsync);
	__dev_mc_sync(netdev, iavf_addr_sync, iavf_addr_unsync);

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	if (netdev->flags & IFF_PROMISC &&
	    !(adapter->flags & IAVF_FLAG_PROMISC_ON))
		adapter->aq_required |= IAVF_FLAG_AQ_REQUEST_PROMISC;
	else if (!(netdev->flags & IFF_PROMISC) &&
		 adapter->flags & IAVF_FLAG_PROMISC_ON)
		adapter->aq_required |= IAVF_FLAG_AQ_RELEASE_PROMISC;

	if (netdev->flags & IFF_ALLMULTI &&
	    !(adapter->flags & IAVF_FLAG_ALLMULTI_ON))
		adapter->aq_required |= IAVF_FLAG_AQ_REQUEST_ALLMULTI;
	else if (!(netdev->flags & IFF_ALLMULTI) &&
		 adapter->flags & IAVF_FLAG_ALLMULTI_ON)
		adapter->aq_required |= IAVF_FLAG_AQ_RELEASE_ALLMULTI;
}

/**
 * iavf_napi_enable_all - enable NAPI on all queue vectors
 * @adapter: board private structure
 **/
static void iavf_napi_enable_all(struct iavf_adapter *adapter)
{
	int q_idx;
	struct iavf_q_vector *q_vector;
	int q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	for (q_idx = 0; q_idx < q_vectors; q_idx++) {
		struct napi_struct *napi;

		q_vector = &adapter->q_vectors[q_idx];
		napi = &q_vector->napi;
		napi_enable(napi);
	}
}

/**
 * iavf_napi_disable_all - disable NAPI on all queue vectors
 * @adapter: board private structure
 **/
static void iavf_napi_disable_all(struct iavf_adapter *adapter)
{
	int q_vectors = adapter->num_msix_vectors - NONQ_VECS;
	struct iavf_q_vector *q_vector;
	int q_idx;

	for (q_idx = 0; q_idx < q_vectors; q_idx++) {
		q_vector = &adapter->q_vectors[q_idx];
		napi_disable(&q_vector->napi);
	}
}

/**
 * iavf_configure - set up transmit and receive data structures
 * @adapter: board private structure
 **/
static void iavf_configure(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	iavf_set_rx_mode(netdev);

	iavf_configure_tx(adapter);
	iavf_configure_rx(adapter);
	adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_QUEUES;

	for (i = 0; i < adapter->num_active_queues; i++) {
		struct iavf_ring *ring = &adapter->rx_rings[i];

		iavf_alloc_rx_buffers(ring, IAVF_DESC_UNUSED(ring));
	}
}

/**
 * iavf_up_complete - Finish the last steps of bringing up a connection
 * @adapter: board private structure
 *
 * Expects to be called while holding the __IAVF_IN_CRITICAL_TASK bit lock.
 **/
static void iavf_up_complete(struct iavf_adapter *adapter)
{
#ifdef CONFIG_NETDEVICES_MULTIQUEUE
	if (adapter->num_active_queues > 1)
		netdev->features |= NETIF_F_MULTI_QUEUE;

#endif
	iavf_change_state(adapter, __IAVF_RUNNING);
	clear_bit(__IAVF_VSI_DOWN, adapter->vsi.state);

	iavf_napi_enable_all(adapter);

	adapter->aq_required |= IAVF_FLAG_AQ_ENABLE_QUEUES;
	if (CLIENT_ENABLED(adapter))
		adapter->flags |= IAVF_FLAG_CLIENT_NEEDS_OPEN;
	mod_delayed_work(iavf_wq, &adapter->watchdog_task, 0);
}

/**
 * iavf_down - Shutdown the connection processing
 * @adapter: board private structure
 *
 * Expects to be called while holding the __IAVF_IN_CRITICAL_TASK bit lock.
 **/
void iavf_down(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct iavf_vlan_filter *vlf;
	struct iavf_cloud_filter *cf;
	struct iavf_mac_filter *f;

	if (adapter->state <= __IAVF_DOWN_PENDING)
		return;

	netif_carrier_off(netdev);
	netif_tx_disable(netdev);
	adapter->link_up = false;
	iavf_irq_disable(adapter);
	iavf_napi_disable_all(adapter);

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	/* clear the sync flag on all filters */
	__dev_uc_unsync(adapter->netdev, NULL);
	__dev_mc_unsync(adapter->netdev, NULL);

	/* remove all MAC filters */
	list_for_each_entry(f, &adapter->mac_filter_list, list) {
		f->remove = true;
	}

	/* remove all VLAN filters */
	list_for_each_entry(vlf, &adapter->vlan_filter_list, list) {
		vlf->remove = true;
	}

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	/* remove all cloud filters */
	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_for_each_entry(cf, &adapter->cloud_filter_list, list) {
		cf->del = true;
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);

	if (!(adapter->flags & IAVF_FLAG_PF_COMMS_FAILED) &&
	    adapter->state != __IAVF_RESETTING) {
		/* cancel any current operation */
		adapter->current_op = VIRTCHNL_OP_UNKNOWN;
		/* Schedule operations to close down the HW. Don't wait
		 * here for this to complete. The watchdog is still running
		 * and it will take care of this.
		 */
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_MAC_FILTER;
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_VLAN_FILTER;
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_CLOUD_FILTER;
		adapter->aq_required |= IAVF_FLAG_AQ_DISABLE_QUEUES;
		/* In case the queue configure or enable operations are still
		 * pending from when the interface was opened, make sure
		 * they're canceled here.
		 */
		adapter->aq_required &= ~IAVF_FLAG_AQ_ENABLE_QUEUES;
		adapter->aq_required &= ~IAVF_FLAG_AQ_CONFIGURE_QUEUES;
	}

	mod_delayed_work(iavf_wq, &adapter->watchdog_task, 0);
}

/**
 * iavf_acquire_msix_vectors - Setup the MSIX capability
 * @adapter: board private structure
 * @vectors: number of vectors to request
 *
 * Work with the OS to set up the MSIX vectors needed.
 *
 * Returns 0 on success, negative on failure
 **/
static int
iavf_acquire_msix_vectors(struct iavf_adapter *adapter, int vectors)
{
	int v_actual;

	/* We'll want at least 3 (vector_threshold):
	 * 0) Other (Admin Queue and link, mostly)
	 * 1) TxQ[0] Cleanup
	 * 2) RxQ[0] Cleanup
	 *
	 * The more we get, the more we will assign to Tx/Rx Cleanup
	 * for the separate queues...where Rx Cleanup >= Tx Cleanup.
	 * Right now, we simply care about how many we'll get; we'll
	 * set them up later while requesting irq's.
	 */
	v_actual = pci_enable_msix_range(adapter->pdev, adapter->msix_entries,
					 MIN_MSIX_COUNT, vectors);
	if (v_actual < 0) {
		dev_err(&adapter->pdev->dev, "Unable to allocate MSI-X interrupts: %d\n",
			v_actual);
		kfree(adapter->msix_entries);
		adapter->msix_entries = NULL;
		return v_actual;
	}

	if (CLIENT_ALLOWED(adapter)) {
		adapter->num_iwarp_msix = (v_actual - 1) / 2;
		adapter->num_msix_vectors = v_actual - adapter->num_iwarp_msix;
		adapter->iwarp_base_vector = adapter->num_msix_vectors;
	} else {
		adapter->num_msix_vectors = v_actual;
	}

	return 0;
}

/**
 * iavf_free_queues - Free memory for all rings
 * @adapter: board private structure to initialize
 *
 * Free all of the memory associated with queue pairs.
 **/
static void iavf_free_queues(struct iavf_adapter *adapter)
{
	if (!adapter->vsi_res)
		return;
	adapter->num_active_queues = 0;
	kfree(adapter->tx_rings);
	adapter->tx_rings = NULL;
	kfree(adapter->rx_rings);
	adapter->rx_rings = NULL;
}

/**
 * iavf_alloc_queues - Allocate memory for all rings
 * @adapter: board private structure to initialize
 *
 * We allocate one ring per queue at run-time since we don't know the
 * number of queues at compile-time.  The polling_netdev array is
 * intended for Multiqueue, but should work fine with a single queue.
 **/
static int iavf_alloc_queues(struct iavf_adapter *adapter)
{
	int i, num_active_queues;

	/* If we're in reset reallocating queues we don't actually know yet for
	 * certain the PF gave us the number of queues we asked for but we'll
	 * assume it did.  Once basic reset is finished we'll confirm once we
	 * start negotiating config with PF.
	 */
	if (adapter->num_req_queues)
		num_active_queues = adapter->num_req_queues;
#ifdef __TC_MQPRIO_MODE_MAX
	else if ((adapter->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ADQ) &&
		 adapter->num_tc)
		num_active_queues = adapter->ch_config.total_qps;
#endif /* __TC_MQPRIO_MODE_MAX */
	else
		num_active_queues = min_t(int,
					  adapter->vsi_res->num_queue_pairs,
					  (int)(num_online_cpus()));


	adapter->tx_rings = kcalloc(num_active_queues,
				    sizeof(struct iavf_ring), GFP_KERNEL);
	if (!adapter->tx_rings)
		goto err_out;
	adapter->rx_rings = kcalloc(num_active_queues,
				    sizeof(struct iavf_ring), GFP_KERNEL);
	if (!adapter->rx_rings)
		goto err_out;

	for (i = 0; i < num_active_queues; i++) {
		struct iavf_ring *tx_ring;
		struct iavf_ring *rx_ring;

		tx_ring = &adapter->tx_rings[i];

		tx_ring->queue_index = i;
		tx_ring->netdev = adapter->netdev;
		tx_ring->dev = pci_dev_to_dev(adapter->pdev);
		tx_ring->count = adapter->tx_desc_count;
		tx_ring->itr_setting = IAVF_ITR_TX_DEF;

		if (adapter->flags & IAVF_FLAG_WB_ON_ITR_CAPABLE)
			tx_ring->flags |= IAVF_TXR_FLAGS_WB_ON_ITR;

		rx_ring = &adapter->rx_rings[i];
		rx_ring->queue_index = i;
		rx_ring->netdev = adapter->netdev;
		rx_ring->dev = pci_dev_to_dev(adapter->pdev);
		rx_ring->count = adapter->rx_desc_count;
		rx_ring->itr_setting = IAVF_ITR_RX_DEF;
	}

	adapter->num_active_queues = num_active_queues;

	return 0;

err_out:
	iavf_free_queues(adapter);
	return -ENOMEM;
}

/**
 * iavf_set_interrupt_capability - set MSI-X or FAIL if not supported
 * @adapter: board private structure to initialize
 *
 * Attempt to configure the interrupts using the best available
 * capabilities of the hardware and the kernel.
 **/
static int iavf_set_interrupt_capability(struct iavf_adapter *adapter)
{
	int vector, v_budget;
	int pairs = 0;
	int err = 0;

	if (!adapter->vsi_res) {
		err = -EIO;
		goto out;
	}
	pairs = adapter->num_active_queues;

	/* It's easy to be greedy for MSI-X vectors, but it really doesn't do
	 * us much good if we have more vectors than CPUs. However, we already
	 * limit the total number of queues by the number of CPUs so we do not
	 * need any further limiting here.
	 */
	v_budget = min_t(int, pairs + NONQ_VECS,
			 (int)adapter->vf_res->max_vectors);

	adapter->msix_entries = kcalloc(v_budget,
					sizeof(struct msix_entry), GFP_KERNEL);
	if (!adapter->msix_entries) {
		err = -ENOMEM;
		goto out;
	}

	for (vector = 0; vector < v_budget; vector++)
		adapter->msix_entries[vector].entry = vector;

	err = iavf_acquire_msix_vectors(adapter, v_budget);
out:
#ifdef CONFIG_NETDEVICES_MULTIQUEUE
	/* Notify the stack of the (possibly) reduced Tx Queue count. */
	adapter->netdev->egress_subqueue_count = pairs;
#else /* CONFIG_NETDEVICES_MULTIQUEUE */
	netif_set_real_num_rx_queues(adapter->netdev, pairs);
	netif_set_real_num_tx_queues(adapter->netdev, pairs);
#endif /* CONFIG_NETDEVICES_MULTIQUEUE */
	return err;
}

/**
 * iavf_alloc_q_vectors - Allocate memory for interrupt vectors
 * @adapter: board private structure to initialize
 *
 * We allocate one q_vector per queue interrupt.  If allocation fails we
 * return -ENOMEM.
 **/
static int iavf_alloc_q_vectors(struct iavf_adapter *adapter)
{
	int q_idx = 0, num_q_vectors;
	struct iavf_q_vector *q_vector;

	num_q_vectors = adapter->num_msix_vectors - NONQ_VECS;
	adapter->q_vectors = kcalloc(num_q_vectors, sizeof(*q_vector),
				     GFP_KERNEL);
	if (!adapter->q_vectors)
		return -ENOMEM;
	for (q_idx = 0; q_idx < num_q_vectors; q_idx++) {
		q_vector = &adapter->q_vectors[q_idx];
		q_vector->adapter = adapter;
		q_vector->vsi = &adapter->vsi;
		q_vector->v_idx = q_idx;
		q_vector->reg_idx = q_idx;
#ifdef HAVE_IRQ_AFFINITY_NOTIFY
		cpumask_copy(&q_vector->affinity_mask, cpu_possible_mask);
#endif
		netif_napi_add(adapter->netdev, &q_vector->napi,
			       iavf_napi_poll, NAPI_POLL_WEIGHT);
	}

	return 0;
}

/**
 * iavf_free_q_vectors - Free memory allocated for interrupt vectors
 * @adapter: board private structure to initialize
 *
 * This function frees the memory allocated to the q_vectors.  In addition if
 * NAPI is enabled it will delete any references to the NAPI struct prior
 * to freeing the q_vector.
 **/
static void iavf_free_q_vectors(struct iavf_adapter *adapter)
{
	int q_idx, num_q_vectors;
	int napi_vectors;

	if (!adapter->q_vectors)
		return;

	num_q_vectors = adapter->num_msix_vectors - NONQ_VECS;
	napi_vectors = adapter->num_active_queues;

	for (q_idx = 0; q_idx < num_q_vectors; q_idx++) {
		struct iavf_q_vector *q_vector = &adapter->q_vectors[q_idx];
		if (q_idx < napi_vectors)
			netif_napi_del(&q_vector->napi);
	}
	kfree(adapter->q_vectors);
	adapter->q_vectors = NULL;
}

/**
 * iavf_reset_interrupt_capability - Reset MSIX setup
 * @adapter: board private structure
 *
 **/
void iavf_reset_interrupt_capability(struct iavf_adapter *adapter)
{
	if (!adapter->msix_entries)
		return;

	pci_disable_msix(adapter->pdev);
	kfree(adapter->msix_entries);
	adapter->msix_entries = NULL;
}

/**
 * iavf_init_interrupt_scheme - Determine if MSIX is supported and init
 * @adapter: board private structure to initialize
 *
 **/
int iavf_init_interrupt_scheme(struct iavf_adapter *adapter)
{
	int err;

	err = iavf_alloc_queues(adapter);
	if (err) {
		dev_err(&adapter->pdev->dev,
			"Unable to allocate memory for queues\n");
		goto err_alloc_queues;
	}

	rtnl_lock();
	err = iavf_set_interrupt_capability(adapter);
	rtnl_unlock();
	if (err) {
		dev_err(&adapter->pdev->dev,
			"Unable to setup interrupt capabilities\n");
		goto err_set_interrupt;
	}

	err = iavf_alloc_q_vectors(adapter);
	if (err) {
		dev_err(&adapter->pdev->dev,
			"Unable to allocate memory for queue vectors\n");
		goto err_alloc_q_vectors;
	}

#ifdef __TC_MQPRIO_MODE_MAX
	/* If we've made it so far while ADq flag being ON, then we haven't
	 * bailed out anywhere in middle. And ADq isn't just enabled but actual
	 * resources have been allocated in the reset path.
	 * Now we can truly claim that ADq is enabled.
	 */
	if ((adapter->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ADQ) &&
	    adapter->num_tc)
		dev_info(&adapter->pdev->dev, "ADq Enabled, %u TCs created",
			 adapter->num_tc);
#endif /* __TC_MQPRIO_MODE_MAX */

	dev_info(&adapter->pdev->dev, "Multiqueue %s: Queue pair count = %u",
		 (adapter->num_active_queues > 1) ? "Enabled" : "Disabled",
		 adapter->num_active_queues);

	return 0;
err_alloc_q_vectors:
	iavf_reset_interrupt_capability(adapter);
err_set_interrupt:
	iavf_free_queues(adapter);
err_alloc_queues:
	return err;
}

/**
 * iavf_config_rss_aq - Configure RSS keys and lut by using AQ commands
 * @adapter: board private structure
 *
 * Return 0 on success, negative on failure
 **/
static int iavf_config_rss_aq(struct iavf_adapter *adapter)
{
	struct iavf_aqc_get_set_rss_key_data *rss_key =
		(struct iavf_aqc_get_set_rss_key_data *)adapter->rss_key;
	struct iavf_hw *hw = &adapter->hw;
	int ret = 0;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev,
			"Cannot configure RSS, command %d pending\n",
			adapter->current_op);
		return -EBUSY;
	}

	ret = iavf_aq_set_rss_key(hw, adapter->vsi.id, rss_key);
	if (ret) {
		dev_err(&adapter->pdev->dev, "Cannot set RSS key, err %s aq_err %s\n",
			iavf_stat_str(hw, ret),
			iavf_aq_str(hw, hw->aq.asq_last_status));
		return ret;
	}


	ret = iavf_aq_set_rss_lut(hw, adapter->vsi.id, false,
				  adapter->rss_lut, adapter->rss_lut_size);
	if (ret) {
		dev_err(&adapter->pdev->dev, "Cannot set RSS lut, err %s aq_err %s\n",
			iavf_stat_str(hw, ret),
			iavf_aq_str(hw, hw->aq.asq_last_status));
	}

	return ret;
}

/**
 * iavf_config_rss_reg - Configure RSS keys and lut by writing registers
 * @adapter: board private structure
 *
 * Returns 0 on success, negative on failure
 **/
static int iavf_config_rss_reg(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;
	u32 *dw;
	u16 i;

	dw = (u32 *)adapter->rss_key;
	for (i = 0; i <= adapter->rss_key_size / 4; i++)
		wr32(hw, IAVF_VFQF_HKEY(i), dw[i]);

	dw = (u32 *)adapter->rss_lut;
	for (i = 0; i <= adapter->rss_lut_size / 4; i++)
		wr32(hw, IAVF_VFQF_HLUT(i), dw[i]);

	iavf_flush(hw);

	return 0;
}

/**
 * iavf_config_rss - Configure RSS keys and lut
 * @adapter: board private structure
 *
 * Returns 0 on success, negative on failure
 **/
int iavf_config_rss(struct iavf_adapter *adapter)
{

	if (RSS_PF(adapter)) {
		adapter->aq_required |= IAVF_FLAG_AQ_SET_RSS_LUT |
					IAVF_FLAG_AQ_SET_RSS_KEY;
		return 0;
	} else if (RSS_AQ(adapter)) {
		return iavf_config_rss_aq(adapter);
	} else {
		return iavf_config_rss_reg(adapter);
	}
}

/**
 * iavf_fill_rss_lut - Fill the lut with default values
 * @adapter: board private structure
 **/
static void iavf_fill_rss_lut(struct iavf_adapter *adapter)
{
	u16 i;

	for (i = 0; i < adapter->rss_lut_size; i++)
		adapter->rss_lut[i] = i % adapter->num_active_queues;
}

/**
 * iavf_init_rss - Prepare for RSS
 * @adapter: board private structure
 *
 * Return 0 on success, negative on failure
 **/
static int iavf_init_rss(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;
	int ret;

	if (!RSS_PF(adapter)) {
		/* Enable PCTYPES for RSS, TCP/UDP with IPv4/IPv6 */
		if (adapter->vf_res->vf_cap_flags &
		    VIRTCHNL_VF_OFFLOAD_RSS_PCTYPE_V2)
			adapter->hena = IAVF_DEFAULT_RSS_HENA_EXPANDED;
		else
			adapter->hena = IAVF_DEFAULT_RSS_HENA;

		wr32(hw, IAVF_VFQF_HENA(0), (u32)adapter->hena);
		wr32(hw, IAVF_VFQF_HENA(1), (u32)(adapter->hena >> 32));
	}

	iavf_fill_rss_lut(adapter);
	netdev_rss_key_fill((void *)adapter->rss_key, adapter->rss_key_size);
	ret = iavf_config_rss(adapter);

	return ret;
}

/**
 * iavf_free_rss - Free memory used by RSS structs
 * @adapter: board private structure
 **/
static void iavf_free_rss(struct iavf_adapter *adapter)
{
	kfree(adapter->rss_key);
	adapter->rss_key = NULL;

	kfree(adapter->rss_lut);
	adapter->rss_lut = NULL;
}

/**
 * iavf_reinit_interrupt_scheme - Reallocate queues and vectors
 * @adapter: board private structure
 *
 * Returns 0 on success, negative on failure
 **/
static int iavf_reinit_interrupt_scheme(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int err;

	if (!test_bit(__IAVF_VSI_DOWN, adapter->vsi.state))
		iavf_free_traffic_irqs(adapter);
	iavf_free_misc_irq(adapter);
	iavf_reset_interrupt_capability(adapter);
	iavf_free_q_vectors(adapter);
	iavf_free_queues(adapter);

	err =  iavf_init_interrupt_scheme(adapter);
	if (err)
		goto err;

	netif_tx_stop_all_queues(netdev);

	err = iavf_request_misc_irq(adapter);
	if (err)
		goto err;

	set_bit(__IAVF_VSI_DOWN, adapter->vsi.state);

	iavf_map_rings_to_vectors(adapter);

	if (RSS_AQ(adapter))
		adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_RSS;
	else
		err = iavf_init_rss(adapter);
err:
	return err;
}

/**
 * iavf_startup - first step of driver startup
 * @adapter: board private structure
 *
 * Function process __IAVF_STARTUP driver state.
 * When success the state is changed to __IAVF_INIT_VERSION_CHECK
 * when fails the state is changed to __IAVF_INIT_FAILED
 **/
static void iavf_startup(struct iavf_adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;
	struct iavf_hw *hw = &adapter->hw;
	int ret;

	WARN_ON(adapter->state != __IAVF_STARTUP);

	/* driver loaded, probe complete */
	adapter->flags &= ~IAVF_FLAG_PF_COMMS_FAILED;
	adapter->flags &= ~IAVF_FLAG_RESET_PENDING;
	ret = iavf_set_mac_type(hw);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set MAC type (%d)\n", ret);
		goto err;
	}

	ret = iavf_check_reset_complete(hw);
	if (ret) {
		dev_dbg(&pdev->dev, "Device is still in reset (%d), retrying\n",
			ret);
		goto err;
	}

	hw->aq.num_arq_entries = IAVF_AQ_LEN;
	hw->aq.num_asq_entries = IAVF_AQ_LEN;
	hw->aq.arq_buf_size = IAVF_MAX_AQ_BUF_SIZE;
	hw->aq.asq_buf_size = IAVF_MAX_AQ_BUF_SIZE;

	ret = iavf_init_adminq(hw);
	if (ret) {
		dev_err(&pdev->dev, "Failed to init Admin Queue (%d)\n", ret);
		goto err;
	}
	ret = iavf_send_api_ver(adapter);
	if (ret) {
		dev_err(&pdev->dev, "Unable to send to PF (%d)\n", ret);
		iavf_shutdown_adminq(hw);
		goto err;
	}
	iavf_change_state(adapter, __IAVF_INIT_VERSION_CHECK);
	return;
err:
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

/**
 * iavf_init_version_check - second step of driver startup
 * @adapter: board private structure
 *
 * Function process __IAVF_INIT_VERSION_CHECK driver state.
 * When success the state is changed to __IAVF_INIT_GET_RESOURCES
 * when fails the state is changed to __IAVF_INIT_FAILED
 **/
static void iavf_init_version_check(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;
	struct pci_dev *pdev = adapter->pdev;
	int ret;

	WARN_ON(adapter->state != __IAVF_INIT_VERSION_CHECK);

	if (!iavf_asq_done(hw)) {
		dev_err(&pdev->dev, "Admin queue command never completed\n");
		iavf_shutdown_adminq(hw);
		iavf_change_state(adapter, __IAVF_STARTUP);
		goto err;
	}

	/* aq msg sent, awaiting reply */
	ret = iavf_verify_api_ver(adapter);
	if (ret) {
		if (ret == IAVF_ERR_ADMIN_QUEUE_NO_WORK)
			ret = iavf_send_api_ver(adapter);
		else
			dev_err(&pdev->dev, "Unsupported PF API version %d.%d, expected %d.%d\n",
				adapter->pf_version.major,
				adapter->pf_version.minor,
				VIRTCHNL_VERSION_MAJOR,
				VIRTCHNL_VERSION_MINOR);
		goto err;
	}
	ret = iavf_send_vf_config_msg(adapter);
	if (ret) {
		dev_err(&pdev->dev, "Unable to send config request (%d)\n",
			ret);
		goto err;
	}
	iavf_change_state(adapter, __IAVF_INIT_GET_RESOURCES);
	return;
err:
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

/**
 * iavf_init_get_resources - third step of driver startup
 * @adapter: board private structure
 *
 * Function process __IAVF_INIT_GET_RESOURCES driver state and
 * finishes driver initialization procedure.
 * When success the state is changed to __IAVF_DOWN
 * when fails the state is changed to __IAVF_INIT_FAILED
 **/
static void iavf_init_get_resources(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct iavf_hw *hw = &adapter->hw;
	struct pci_dev *pdev = adapter->pdev;
	int ret, bufsz;

	WARN_ON(adapter->state != __IAVF_INIT_GET_RESOURCES);
	/* aq msg sent, awaiting reply */
	if (!adapter->vf_res) {
		bufsz = sizeof(struct virtchnl_vf_resource) +
			(IAVF_MAX_VF_VSI *
			sizeof(struct virtchnl_vsi_resource));
		adapter->vf_res = kzalloc(bufsz, GFP_KERNEL);
		if (!adapter->vf_res)
			goto err;
	}
	ret = iavf_get_vf_config(adapter);
	if (ret == IAVF_ERR_ADMIN_QUEUE_NO_WORK) {
		ret = iavf_send_vf_config_msg(adapter);
		goto err;
	} else if (ret == IAVF_ERR_PARAM) {
		/* We only get ERR_PARAM if the device is in a very bad
		 * state or if we've been disabled for previous bad
		 * behavior. Either way, we're done now.
		 */
		iavf_shutdown_adminq(hw);
		dev_err(&pdev->dev, "Unable to get VF config due to PF error condition, not retrying\n");
		return;
	}
	if (ret) {
		dev_err(&pdev->dev, "Unable to get VF config (%d)\n", ret);
		goto err_alloc;
	}

	if (iavf_process_config(adapter))
		goto err_alloc;
	adapter->current_op = VIRTCHNL_OP_UNKNOWN;

	adapter->flags |= IAVF_FLAG_RX_CSUM_ENABLED;

#ifndef HAVE_SWIOTLB_SKIP_CPU_SYNC
	/* force legacy Rx mode if SKIP_CPU_SYNC is not supported */
	adapter->flags |= IAVF_FLAG_LEGACY_RX;
#endif
	netdev->netdev_ops = &iavf_netdev_ops;
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
	set_netdev_ops_ext(netdev, &iavf_netdev_ops_ext);
#endif
	iavf_set_ethtool_ops(netdev);
	netdev->watchdog_timeo = 5 * HZ;

#ifdef HAVE_NETDEVICE_MIN_MAX_MTU
	/* MTU range: 68 - 9710 */
#ifdef HAVE_RHEL7_EXTENDED_MIN_MAX_MTU
	netdev->extended->min_mtu = ETH_MIN_MTU;
	netdev->extended->max_mtu = IAVF_MAX_RXBUFFER - IAVF_PACKET_HDR_PAD;
#else
	netdev->min_mtu = ETH_MIN_MTU;
	netdev->max_mtu = IAVF_MAX_RXBUFFER - IAVF_PACKET_HDR_PAD;
#endif /* HAVE_RHEL7_EXTENDED_MIN_MAX_MTU */
#endif /* HAVE_NETDEVICE_MIN_MAX_NTU */

	if (!is_valid_ether_addr(adapter->hw.mac.addr)) {
		dev_info(&pdev->dev, "Invalid MAC address %pM, using random\n",
			 adapter->hw.mac.addr);
		eth_hw_addr_random(netdev);
		ether_addr_copy(adapter->hw.mac.addr, netdev->dev_addr);
	} else {
		adapter->flags |= IAVF_FLAG_ADDR_SET_BY_PF;
		ether_addr_copy(netdev->dev_addr, adapter->hw.mac.addr);
		ether_addr_copy(netdev->perm_addr, adapter->hw.mac.addr);
	}

	adapter->tx_desc_count = IAVF_DEFAULT_TXD;
	adapter->rx_desc_count = IAVF_DEFAULT_RXD;
	ret = iavf_init_interrupt_scheme(adapter);
	if (ret)
		goto err_sw_init;
	iavf_map_rings_to_vectors(adapter);
	if (adapter->vf_res->vf_cap_flags &
		VIRTCHNL_VF_OFFLOAD_WB_ON_ITR)
		adapter->flags |= IAVF_FLAG_WB_ON_ITR_CAPABLE;

	ret = iavf_request_misc_irq(adapter);
	if (ret)
		goto err_sw_init;

	netif_carrier_off(netdev);
	adapter->link_up = false;

	if (!adapter->netdev_registered) {
		ret = register_netdev(netdev);
		if (ret)
			goto err_register;
	}

	adapter->netdev_registered = true;

	netif_tx_stop_all_queues(netdev);
	if (CLIENT_ALLOWED(adapter)) {
		ret = iavf_lan_add_device(adapter);
		if (ret)
			dev_info(&pdev->dev, "Failed to add VF to client API service list: %d\n",
				 ret);
	}
	dev_info(&pdev->dev, "MAC address: %pM\n", adapter->hw.mac.addr);
	if (netdev->features & NETIF_F_GRO)
		dev_info(&pdev->dev, "GRO is enabled\n");

	iavf_change_state(adapter, __IAVF_DOWN);
	set_bit(__IAVF_VSI_DOWN, adapter->vsi.state);

	iavf_misc_irq_enable(adapter);
	wake_up(&adapter->down_waitqueue);

	adapter->rss_key = kzalloc(adapter->rss_key_size, GFP_KERNEL);
	adapter->rss_lut = kzalloc(adapter->rss_lut_size, GFP_KERNEL);
	if (!adapter->rss_key || !adapter->rss_lut)
		goto err_mem;
	if (RSS_AQ(adapter))
		adapter->aq_required |= IAVF_FLAG_AQ_CONFIGURE_RSS;
	else
		iavf_init_rss(adapter);

	return;
err_mem:
	iavf_free_rss(adapter);
err_register:
	iavf_free_misc_irq(adapter);
err_sw_init:
	iavf_reset_interrupt_capability(adapter);
err_alloc:
	kfree(adapter->vf_res);
	adapter->vf_res = NULL;
err:
	iavf_change_state(adapter, __IAVF_INIT_FAILED);
}

/**
 * iavf_process_aq_command - process aq_required flags
 * and sends aq command
 * @adapter: pointer to iavf adapter structure
 *
 * Returns 0 on success
 * Returns error code if no command was sent
 * or error code if the command failed.
 **/
static int iavf_process_aq_command(struct iavf_adapter *adapter)
{
	if (adapter->aq_required & IAVF_FLAG_AQ_GET_CONFIG)
		return iavf_send_vf_config_msg(adapter);
	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_QUEUES) {
		iavf_disable_queues(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_MAP_VECTORS) {
		iavf_map_queues(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ADD_MAC_FILTER) {
		iavf_add_ether_addrs(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ADD_VLAN_FILTER) {
		iavf_add_vlans(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DEL_MAC_FILTER) {
		iavf_del_ether_addrs(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DEL_VLAN_FILTER) {
		iavf_del_vlans(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_VLAN_STRIPPING) {
		iavf_enable_vlan_stripping(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_VLAN_STRIPPING) {
		iavf_disable_vlan_stripping(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_CONFIGURE_QUEUES) {
		iavf_configure_queues(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_QUEUES) {
		iavf_enable_queues(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_CONFIGURE_RSS) {
		/* This message goes straight to the firmware, not the
		 * PF, so we don't have to set current_op as we will
		 * not get a response through the ARQ.
		 */
		adapter->aq_required &= ~IAVF_FLAG_AQ_CONFIGURE_RSS;
		return iavf_init_rss(adapter);
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_GET_HENA) {
		iavf_get_hena(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_SET_HENA) {
		iavf_set_hena(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_SET_RSS_KEY) {
		iavf_set_rss_key(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_SET_RSS_LUT) {
		iavf_set_rss_lut(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_REQUEST_PROMISC) {
		iavf_set_promiscuous(adapter, FLAG_VF_UNICAST_PROMISC |
				       FLAG_VF_MULTICAST_PROMISC);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_REQUEST_ALLMULTI) {
		iavf_set_promiscuous(adapter, FLAG_VF_MULTICAST_PROMISC);
		return IAVF_SUCCESS;
	}
	if ((adapter->aq_required & IAVF_FLAG_AQ_RELEASE_PROMISC) ||
	    (adapter->aq_required & IAVF_FLAG_AQ_RELEASE_ALLMULTI)) {
		iavf_set_promiscuous(adapter, 0);
		return IAVF_SUCCESS;
	}
#ifdef __TC_MQPRIO_MODE_MAX
	if (adapter->aq_required & IAVF_FLAG_AQ_ENABLE_CHANNELS) {
		iavf_enable_channels(adapter);
		return IAVF_SUCCESS;
	}

	if (adapter->aq_required & IAVF_FLAG_AQ_DISABLE_CHANNELS) {
		iavf_disable_channels(adapter);
		return IAVF_SUCCESS;
	}
#endif /* __TC_MQPRIO_MODE_MAX */
	if (adapter->aq_required & IAVF_FLAG_AQ_DEL_CLOUD_FILTER) {
		iavf_del_cloud_filter(adapter);
		return IAVF_SUCCESS;
	}
	if (adapter->aq_required & IAVF_FLAG_AQ_ADD_CLOUD_FILTER) {
		iavf_add_cloud_filter(adapter);
		return IAVF_SUCCESS;
	}
	return -EAGAIN;
}

/**
 * iavf_watchdog_task - Periodic call-back task
 * @work: pointer to work_struct
 **/
static void iavf_watchdog_task(struct work_struct *work)
{
	struct iavf_adapter *adapter = container_of(work,
						    struct iavf_adapter,
						    watchdog_task.work);
	struct iavf_hw *hw = &adapter->hw;
	u32 reg_val;

	if (test_and_set_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section))
		goto restart_watchdog;

	if (adapter->flags & IAVF_FLAG_PF_COMMS_FAILED)
		iavf_change_state(adapter, __IAVF_COMM_FAILED);

	if (adapter->flags & IAVF_FLAG_RESET_NEEDED &&
	    adapter->state != __IAVF_RESETTING) {
		iavf_change_state(adapter, __IAVF_RESETTING);
		adapter->aq_required = 0;
		adapter->current_op = VIRTCHNL_OP_UNKNOWN;
	}

	switch (adapter->state) {
	case __IAVF_STARTUP:
		iavf_startup(adapter);
		clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
		queue_delayed_work(iavf_wq, &adapter->watchdog_task,
				   msecs_to_jiffies(30));
		return;
	case __IAVF_INIT_VERSION_CHECK:
		iavf_init_version_check(adapter);
		clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
		queue_delayed_work(iavf_wq, &adapter->watchdog_task,
				   msecs_to_jiffies(30));
		return;
	case __IAVF_INIT_GET_RESOURCES:
		iavf_init_get_resources(adapter);
		clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
		queue_delayed_work(iavf_wq, &adapter->watchdog_task,
				   msecs_to_jiffies(1));
		return;
	case __IAVF_INIT_FAILED:
		if (++adapter->aq_wait_count > IAVF_AQ_MAX_ERR) {
			dev_err(&adapter->pdev->dev,
				"Failed to communicate with PF; waiting before retry\n");
			adapter->flags |= IAVF_FLAG_PF_COMMS_FAILED;
			iavf_shutdown_adminq(hw);
			clear_bit(__IAVF_IN_CRITICAL_TASK,
				  &adapter->crit_section);
			queue_delayed_work(iavf_wq,
					   &adapter->watchdog_task, (5 * HZ));
			return;
		}
		/* Try again from failed step*/
		iavf_change_state(adapter, adapter->last_state);
		clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
		queue_delayed_work(iavf_wq, &adapter->watchdog_task, HZ);
		return;
	case __IAVF_COMM_FAILED:
		reg_val = rd32(hw, IAVF_VFGEN_RSTAT) &
			  IAVF_VFGEN_RSTAT_VFR_STATE_MASK;
		if (reg_val == VIRTCHNL_VFR_VFACTIVE ||
		    reg_val == VIRTCHNL_VFR_COMPLETED) {
			/* A chance for redemption! */
			dev_err(&adapter->pdev->dev,
				"Hardware came out of reset. Attempting reinit.\n");
			/* When init_task contacts the PF and
			 * gets everything set up again, it'll restart the
			 * watchdog for us. Down, boy. Sit. Stay. Woof.
			 */
			iavf_change_state(adapter, __IAVF_STARTUP);
			adapter->flags &= ~IAVF_FLAG_PF_COMMS_FAILED;
		}
		adapter->aq_required = 0;
		adapter->current_op = VIRTCHNL_OP_UNKNOWN;
		clear_bit(__IAVF_IN_CRITICAL_TASK,
			  &adapter->crit_section);
		queue_delayed_work(iavf_wq,
				   &adapter->watchdog_task,
				   msecs_to_jiffies(10));
		return;
	case __IAVF_RESETTING:
		iavf_handle_reset(adapter);
		clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
		queue_delayed_work(iavf_wq,
				   &adapter->watchdog_task,
				   msecs_to_jiffies(2));
		return;
	case __IAVF_DOWN:
	case __IAVF_DOWN_PENDING:
	case __IAVF_TESTING:
	case __IAVF_RUNNING:
		if (adapter->current_op) {
			if (!iavf_asq_done(hw)) {
				dev_dbg(&adapter->pdev->dev,
					"Admin queue timeout\n");
				iavf_send_api_ver(adapter);
			}
		} else {
			if (iavf_process_aq_command(adapter) &&
			    adapter->state == __IAVF_RUNNING)
				iavf_request_stats(adapter);
		}
		if (adapter->state == __IAVF_RUNNING)
			iavf_detect_recover_hung(&adapter->vsi);
		break;
	case __IAVF_REMOVE:
		clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
		return;
	default:
		break;
	}

	/* check for hw reset */
	reg_val = rd32(hw, IAVF_VF_ARQLEN1) & IAVF_VF_ARQLEN1_ARQENABLE_MASK;
	if (!reg_val) {
		iavf_change_state(adapter, __IAVF_RESETTING);
		iavf_schedule_reset(adapter);
		adapter->aq_required = 0;
		adapter->current_op = VIRTCHNL_OP_UNKNOWN;
		dev_err(&adapter->pdev->dev, "Hardware reset detected\n");
		clear_bit(__IAVF_IN_CRITICAL_TASK,
			  &adapter->crit_section);
		queue_work(iavf_wq, &adapter->watchdog_task.work);
		return;
	}

	schedule_delayed_work(&adapter->client_task, msecs_to_jiffies(5));

	clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
restart_watchdog:
	queue_work(iavf_wq, &adapter->adminq_task);
	if (adapter->aq_required)
		queue_delayed_work(iavf_wq, &adapter->watchdog_task,
				   msecs_to_jiffies(20));
	else
		queue_delayed_work(iavf_wq, &adapter->watchdog_task, HZ * 2);
}

/**
 * iavf_disable_vf - disable a VF that failed to reset
 * @adapter: private adapter structure
 *
 * Helper function to shut down the VF when a reset never finishes.
 **/
static void iavf_disable_vf(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct iavf_mac_filter *f, *ftmp;
	struct iavf_vlan_filter *fv, *fvtmp;
	struct iavf_cloud_filter *cf, *cftmp;
	/* reset never finished */

	adapter->flags |= IAVF_FLAG_PF_COMMS_FAILED;

	/* We don't use netif_running() because it may be true prior to
	 * ndo_open() returning, so we can't assume it means all our open
	 * tasks have finished, since we're not holding the rtnl_lock here.
	 */
	if (!test_bit(__IAVF_VSI_DOWN, adapter->vsi.state)) {
		set_bit(__IAVF_VSI_DOWN, adapter->vsi.state);
		netif_carrier_off(netdev);
		netif_tx_disable(netdev);
		adapter->link_up = false;
		iavf_irq_disable(adapter);
		iavf_napi_disable_all(adapter);
		iavf_free_traffic_irqs(adapter);
		iavf_free_all_tx_resources(adapter);
		iavf_free_all_rx_resources(adapter);
	}

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	/* Delete all of the filters */
	list_for_each_entry_safe(f, ftmp, &adapter->mac_filter_list,
				 list) {
		list_del(&f->list);
		kfree(f);
	}

	list_for_each_entry_safe(fv, fvtmp, &adapter->vlan_filter_list,
				 list) {
		list_del(&fv->list);
		kfree(fv);
	}

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_for_each_entry_safe(cf, cftmp, &adapter->cloud_filter_list, list) {
		list_del(&cf->list);
		kfree(cf);
		adapter->num_cloud_filters--;
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);

	iavf_free_misc_irq(adapter);
	iavf_reset_interrupt_capability(adapter);
	iavf_free_q_vectors(adapter);
	iavf_free_queues(adapter);
	kfree(adapter->vf_res);
	adapter->vf_res = NULL;
	iavf_shutdown_adminq(&adapter->hw);
	adapter->netdev->flags &= ~IFF_UP;
	adapter->flags &= ~IAVF_FLAG_RESET_PENDING;
	adapter->state = __IAVF_DOWN;
	clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
	clear_bit(__IAVF_IN_CLIENT_TASK, &adapter->crit_section);
	adapter->flags &= ~IAVF_FLAG_RESET_PENDING;
	iavf_change_state(adapter, __IAVF_DOWN);
	wake_up(&adapter->down_waitqueue);
	dev_info(&adapter->pdev->dev, "Reset task did not complete, VF disabled\n");
}

#define IAVF_RESET_WAIT_MS 10
#define IAVF_RESET_WAIT_COUNT 500
/**
 * iavf_handle_reset - Handle hardware reset
 * @adapter: pointer to iavf_adapter
 *
 * During reset we need to shut down and reinitialize the admin queue
 * before we can use it to communicate with the PF again. We also clear
 * and reinit the rings because that context is lost as well.
 **/
static void iavf_handle_reset(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct iavf_hw *hw = &adapter->hw;
	u32 reg_val;
	int i = 0, err;
	bool running;

	adapter->flags |= IAVF_FLAG_RESET_PENDING;
	adapter->flags &= ~IAVF_FLAG_RESET_NEEDED;
	while (test_and_set_bit(__IAVF_IN_CLIENT_TASK,
				&adapter->crit_section))
		usleep_range(500, 1000);
	if (CLIENT_ENABLED(adapter)) {
		adapter->flags &= ~(IAVF_FLAG_CLIENT_NEEDS_OPEN |
				    IAVF_FLAG_CLIENT_NEEDS_CLOSE |
				    IAVF_FLAG_CLIENT_NEEDS_L2_PARAMS |
				    IAVF_FLAG_SERVICE_CLIENT_REQUESTED);
		cancel_delayed_work_sync(&adapter->client_task);
		iavf_notify_client_close(&adapter->vsi, true);
	}
	iavf_misc_irq_disable(adapter);
	adapter->flags &= ~IAVF_FLAG_QUEUES_ENABLED;

	/* Restart the AQ here. If we have been reset but didn't
	 * detect it, or if the PF had to reinit, our AQ will be hosed.
	 */
	iavf_shutdown_adminq(hw);
	iavf_init_adminq(hw);
	iavf_request_reset(adapter);

	/* poll until we see the reset actually happen */
	for (i = 0; i < IAVF_RESET_WAIT_COUNT; i++) {
		reg_val = rd32(hw, IAVF_VF_ARQLEN1) &
			  IAVF_VF_ARQLEN1_ARQENABLE_MASK;
		if (!reg_val)
			break;
		usleep_range(5000, 10000);
	}
	if (i == IAVF_RESET_WAIT_COUNT) {
		dev_info(&adapter->pdev->dev, "Never saw reset\n");
		goto continue_reset; /* act like the reset happened */
	}

	/* wait until the reset is complete and the PF is responding to us */
	for (i = 0; i < IAVF_RESET_WAIT_COUNT; i++) {
		/* sleep first to make sure a minimum wait time is met */
		msleep(IAVF_RESET_WAIT_MS);

		reg_val = rd32(hw, IAVF_VFGEN_RSTAT) &
			  IAVF_VFGEN_RSTAT_VFR_STATE_MASK;
		if (reg_val == VIRTCHNL_VFR_VFACTIVE)
			break;
	}

	pci_set_master(adapter->pdev);

	if (i == IAVF_RESET_WAIT_COUNT) {
		dev_err(&adapter->pdev->dev, "Reset never finished (%x)\n",
			reg_val);
		iavf_disable_vf(adapter);
		return; /* Do not attempt to reinit. It's dead, Jim. */
	}

continue_reset:
	iavf_irq_disable(adapter);

	/* We don't use netif_running() because it may be true prior to
	 * ndo_open() returning, so we can't assume it means all our open
	 * tasks have finished, since we're not holding the rtnl_lock here.
	 */
	running = (adapter->last_state == __IAVF_RUNNING);

	if (running) {
		netif_carrier_off(netdev);
		netif_tx_stop_all_queues(netdev);
		adapter->link_up = false;
		iavf_napi_disable_all(adapter);
	}

	adapter->flags &= ~IAVF_FLAG_RESET_PENDING;

	/* free the Tx/Rx rings and descriptors, might be better to just
	 * re-use them sometime in the future
	 */
	iavf_free_all_rx_resources(adapter);
	iavf_free_all_tx_resources(adapter);

	/* kill and reinit the admin queue */
	iavf_shutdown_adminq(hw);
	adapter->current_op = VIRTCHNL_OP_UNKNOWN;
	err = iavf_init_adminq(hw);
	if (err)
		dev_info(&adapter->pdev->dev, "Failed to init adminq: %d\n",
			 err);
	adapter->aq_required = 0;

	if (adapter->flags & IAVF_FLAG_REINIT_ITR_NEEDED) {
		err = iavf_reinit_interrupt_scheme(adapter);
		if (err)
			goto reset_err;
	}

	adapter->aq_required |= IAVF_FLAG_AQ_GET_CONFIG;
	adapter->aq_required |= IAVF_FLAG_AQ_MAP_VECTORS;

	iavf_misc_irq_enable(adapter);

	/* We were running when the reset started, so we need to restore some
	 * state here.
	 */
	if (running) {
		/* allocate transmit descriptors */
		err = iavf_setup_all_tx_resources(adapter);
		if (err)
			goto reset_err;

		/* allocate receive descriptors */
		err = iavf_setup_all_rx_resources(adapter);
		if (err)
			goto reset_err;

		if (adapter->flags & IAVF_FLAG_REINIT_ITR_NEEDED) {
			err = iavf_request_traffic_irqs(adapter, netdev->name);
			if (err)
				goto reset_err;

			adapter->flags &= ~IAVF_FLAG_REINIT_ITR_NEEDED;
		}

		iavf_configure(adapter);

		/* iavf_up_complete() will switch device back
		 * to __IAVF_RUNNING
		 */
		iavf_up_complete(adapter);
	} else {
		iavf_change_state(adapter, __IAVF_DOWN);
		wake_up(&adapter->down_waitqueue);
	}

	clear_bit(__IAVF_IN_CLIENT_TASK, &adapter->crit_section);
	return;
reset_err:
	if (running)
		iavf_change_state(adapter, __IAVF_RUNNING);
	clear_bit(__IAVF_IN_CLIENT_TASK, &adapter->crit_section);
	dev_err(&adapter->pdev->dev, "failed to allocate resources during reinit\n");
	iavf_close(netdev);
}

/**
 * iavf_adminq_task - worker thread to clean the admin queue
 * @work: pointer to work_struct containing our data
 **/
static void iavf_adminq_task(struct work_struct *work)
{
	struct iavf_adapter *adapter =
		container_of(work, struct iavf_adapter, adminq_task);
	struct iavf_hw *hw = &adapter->hw;
	struct iavf_arq_event_info event;
	enum virtchnl_ops v_op;
	enum iavf_status ret, v_ret;
	u32 val, oldval;
	u16 pending;

	if (adapter->flags & IAVF_FLAG_PF_COMMS_FAILED)
		goto out;

	event.buf_len = IAVF_MAX_AQ_BUF_SIZE;
	event.msg_buf = kzalloc(event.buf_len, GFP_KERNEL);
	if (!event.msg_buf)
		goto out;

	do {
		ret = iavf_clean_arq_element(hw, &event, &pending);
		v_op = (enum virtchnl_ops)le32_to_cpu(event.desc.cookie_high);
		v_ret = (enum iavf_status)le32_to_cpu(event.desc.cookie_low);

		if (ret || !v_op)
			break; /* No event to process or error cleaning ARQ */

		while (test_and_set_bit(__IAVF_IN_CRITICAL_TASK,
					&adapter->crit_section))
			usleep_range(500, 1000);
		iavf_virtchnl_completion(adapter, v_op, v_ret, event.msg_buf,
					 event.msg_len);
		clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
		if (pending != 0)
			memset(event.msg_buf, 0, IAVF_MAX_AQ_BUF_SIZE);
	} while (pending);

	if ((adapter->flags &
	     (IAVF_FLAG_RESET_PENDING | IAVF_FLAG_RESET_NEEDED)) ||
	    adapter->state == __IAVF_RESETTING)
		goto freedom;

	/* check for error indications */
	val = rd32(hw, hw->aq.arq.len);
	if (val == 0xdeadbeef || val == 0xffffffff) /* device in reset */
		goto freedom;
	oldval = val;
	if (val & IAVF_VF_ARQLEN1_ARQVFE_MASK) {
		dev_info(&adapter->pdev->dev, "ARQ VF Error detected\n");
		val &= ~IAVF_VF_ARQLEN1_ARQVFE_MASK;
	}
	if (val & IAVF_VF_ARQLEN1_ARQOVFL_MASK) {
		dev_info(&adapter->pdev->dev, "ARQ Overflow Error detected\n");
		val &= ~IAVF_VF_ARQLEN1_ARQOVFL_MASK;
	}
	if (val & IAVF_VF_ARQLEN1_ARQCRIT_MASK) {
		dev_info(&adapter->pdev->dev, "ARQ Critical Error detected\n");
		val &= ~IAVF_VF_ARQLEN1_ARQCRIT_MASK;
	}
	if (oldval != val)
		wr32(hw, hw->aq.arq.len, val);

	val = rd32(hw, hw->aq.asq.len);
	oldval = val;
	if (val & IAVF_VF_ATQLEN1_ATQVFE_MASK) {
		dev_info(&adapter->pdev->dev, "ASQ VF Error detected\n");
		val &= ~IAVF_VF_ATQLEN1_ATQVFE_MASK;
	}
	if (val & IAVF_VF_ATQLEN1_ATQOVFL_MASK) {
		dev_info(&adapter->pdev->dev, "ASQ Overflow Error detected\n");
		val &= ~IAVF_VF_ATQLEN1_ATQOVFL_MASK;
	}
	if (val & IAVF_VF_ATQLEN1_ATQCRIT_MASK) {
		dev_info(&adapter->pdev->dev, "ASQ Critical Error detected\n");
		val &= ~IAVF_VF_ATQLEN1_ATQCRIT_MASK;
	}
	if (oldval != val)
		wr32(hw, hw->aq.asq.len, val);

freedom:
	kfree(event.msg_buf);
out:
	/* re-enable Admin queue interrupt cause */
	iavf_misc_irq_enable(adapter);
}

/**
 * iavf_client_task - worker thread to perform client work
 * @work: pointer to work_struct containing our data
 *
 * This task handles client interactions. Because client calls can be
 * reentrant, we can't handle them in the watchdog.
 **/
static void iavf_client_task(struct work_struct *work)
{
	struct iavf_adapter *adapter =
		container_of(work, struct iavf_adapter, client_task.work);

	/* If we can't get the client bit, just give up. We'll be rescheduled
	 * later.
	 */

	if (test_and_set_bit(__IAVF_IN_CLIENT_TASK, &adapter->crit_section))
		return;

	if (adapter->flags & IAVF_FLAG_SERVICE_CLIENT_REQUESTED) {
		iavf_client_subtask(adapter);
		adapter->flags &= ~IAVF_FLAG_SERVICE_CLIENT_REQUESTED;
		goto out;
	}
	if (adapter->flags & IAVF_FLAG_CLIENT_NEEDS_L2_PARAMS) {
		iavf_notify_client_l2_params(&adapter->vsi);
		adapter->flags &= ~IAVF_FLAG_CLIENT_NEEDS_L2_PARAMS;
		goto out;
	}
	if (adapter->flags & IAVF_FLAG_CLIENT_NEEDS_CLOSE) {
		iavf_notify_client_close(&adapter->vsi, false);
		adapter->flags &= ~IAVF_FLAG_CLIENT_NEEDS_CLOSE;
		goto out;
	}
	if (adapter->flags & IAVF_FLAG_CLIENT_NEEDS_OPEN) {
		iavf_notify_client_open(&adapter->vsi);
		adapter->flags &= ~IAVF_FLAG_CLIENT_NEEDS_OPEN;
	}
out:
	clear_bit(__IAVF_IN_CLIENT_TASK, &adapter->crit_section);
}

/**
 * iavf_free_all_tx_resources - Free Tx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all transmit software resources
 **/
void iavf_free_all_tx_resources(struct iavf_adapter *adapter)
{
	int i;

	if (!adapter->tx_rings)
		return;

	for (i = 0; i < adapter->num_active_queues; i++)
		if (adapter->tx_rings[i].desc)
			iavf_free_tx_resources(&adapter->tx_rings[i]);
}

/**
 * iavf_setup_all_tx_resources - allocate all queues Tx resources
 * @adapter: board private structure
 *
 * If this function returns with an error, then it's possible one or
 * more of the rings is populated (while the rest are not).  It is the
 * callers duty to clean those orphaned rings.
 *
 * Return 0 on success, negative on failure
 **/
static int iavf_setup_all_tx_resources(struct iavf_adapter *adapter)
{
	int i, err = 0;

	for (i = 0; i < adapter->num_active_queues; i++) {
		adapter->tx_rings[i].count = adapter->tx_desc_count;
		err = iavf_setup_tx_descriptors(&adapter->tx_rings[i]);
		if (!err)
			continue;
		dev_err(&adapter->pdev->dev,
			"Allocation for Tx Queue %u failed\n", i);
		break;
	}

	return err;
}

/**
 * iavf_setup_all_rx_resources - allocate all queues Rx resources
 * @adapter: board private structure
 *
 * If this function returns with an error, then it's possible one or
 * more of the rings is populated (while the rest are not).  It is the
 * callers duty to clean those orphaned rings.
 *
 * Return 0 on success, negative on failure
 **/
static int iavf_setup_all_rx_resources(struct iavf_adapter *adapter)
{
	int i, err = 0;

	for (i = 0; i < adapter->num_active_queues; i++) {
		adapter->rx_rings[i].count = adapter->rx_desc_count;
		err = iavf_setup_rx_descriptors(&adapter->rx_rings[i]);
		if (!err)
			continue;
		dev_err(&adapter->pdev->dev,
			"Allocation for Rx Queue %u failed\n", i);
		break;
	}
	return err;
}

/**
 * iavf_free_all_rx_resources - Free Rx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all receive software resources
 **/
void iavf_free_all_rx_resources(struct iavf_adapter *adapter)
{
	int i;

	if (!adapter->rx_rings)
		return;

	for (i = 0; i < adapter->num_active_queues; i++)
		if (adapter->rx_rings[i].desc)
			iavf_free_rx_resources(&adapter->rx_rings[i]);
}

#ifdef HAVE_SETUP_TC
#ifdef HAVE_NDO_SETUP_TC_REMOVE_TC_TO_NETDEV
#ifdef __TC_MQPRIO_MODE_MAX
/**
 * iavf_validate_tx_bandwidth - validate the max Tx bandwidth
 * @adapter: board private structure
 * @max_tx_rate: max Tx bw for a tc
 **/
static int iavf_validate_tx_bandwidth(struct iavf_adapter *adapter,
				      u64 max_tx_rate)
{
	int speed = 0, ret = 0;

	switch (adapter->link_speed) {
	case IAVF_LINK_SPEED_40GB:
		speed = 40000;
		break;
	case IAVF_LINK_SPEED_25GB:
		speed = 25000;
		break;
	case IAVF_LINK_SPEED_20GB:
		speed = 20000;
		break;
	case IAVF_LINK_SPEED_10GB:
		speed = 10000;
		break;
	case IAVF_LINK_SPEED_1GB:
		speed = 1000;
		break;
	case IAVF_LINK_SPEED_100MB:
		speed = 100;
		break;
	default:
		break;
	}

	if (max_tx_rate > speed) {
		dev_err(&adapter->pdev->dev, "Invalid tx rate specified\n");
		ret = -EINVAL;
	}

	return ret;
}

/**
 * iavf_validate_channel_config - validate queue mapping info
 * @adapter: board private structure
 * @mqprio_qopt: queue parameters
 *
 * This function validates if the config provided by the user to
 * configure queue channels is valid or not. Returns 0 on a valid
 * config.
 **/
static int iavf_validate_ch_config(struct iavf_adapter *adapter,
				   struct tc_mqprio_qopt_offload *mqprio_qopt)
{
	u64 total_max_rate = 0;
	int i, num_qps = 0;
	u64 tx_rate = 0;

	if (mqprio_qopt->qopt.num_tc > IAVF_MAX_TRAFFIC_CLASS ||
	    mqprio_qopt->qopt.num_tc < 1)
		return -EINVAL;

	for (i = 0; i <= mqprio_qopt->qopt.num_tc - 1; i++) {
		if (!mqprio_qopt->qopt.count[i] ||
		    mqprio_qopt->qopt.offset[i] != num_qps)
			return -EINVAL;
		if (mqprio_qopt->min_rate[i]) {
			dev_err(&adapter->pdev->dev,
				"Invalid min tx rate (greater than 0) specified\n");
			return -EINVAL;
		}
		/*convert to Mbps */
		tx_rate = div_u64(mqprio_qopt->max_rate[i],
				  IAVF_MBPS_DIVISOR);
		total_max_rate += tx_rate;
		num_qps += mqprio_qopt->qopt.count[i];
	}
	if (num_qps > IAVF_MAX_REQ_QUEUES)
		return -EINVAL;

	return iavf_validate_tx_bandwidth(adapter, total_max_rate);
}

/**
 * iavf_del_all_cloud_filters - delete all cloud filters
 * on the traffic classes
 * @adapter: board private structure
 *
 * This function will loop through the list of cloud filters and
 * deletes them.
 **/
static void iavf_del_all_cloud_filters(struct iavf_adapter *adapter)
{
	struct iavf_cloud_filter *cf, *cftmp;

	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_for_each_entry_safe(cf, cftmp, &adapter->cloud_filter_list,
				 list) {
		list_del(&cf->list);
		kfree(cf);
		adapter->num_cloud_filters--;
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);
}

/**
 *__iavf_setup_tc - configure multiple traffic classes
 * @netdev: network interface device structure
 * @type_data: tc offload data
 *
 * This function processes the config information provided by the
 * user to configure traffic classes/queue channels and packages the
 * information to request the PF to setup traffic classes.
 *
 * Returns 0 on success.
 **/
static int __iavf_setup_tc(struct net_device *netdev, void *type_data)
{
	struct tc_mqprio_qopt_offload *mqprio_qopt = type_data;
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct virtchnl_vf_resource *vfres = adapter->vf_res;
	u8 num_tc = 0, total_qps = 0;
	int ret = 0, netdev_tc = 0;
	u64 max_tx_rate;
	u16 mode;
	int i;

	num_tc = mqprio_qopt->qopt.num_tc;
	mode = mqprio_qopt->mode;

	/* delete queue_channel */
	if (!mqprio_qopt->qopt.hw) {
		if (adapter->ch_config.state == __IAVF_TC_RUNNING) {
			/* reset the tc configuration */
			netdev_reset_tc(netdev);
			adapter->num_tc = 0;
			netif_tx_stop_all_queues(netdev);
			netif_tx_disable(netdev);
			iavf_del_all_cloud_filters(adapter);
			adapter->aq_required = IAVF_FLAG_AQ_DISABLE_CHANNELS;
			goto exit;
		} else {
			return -EINVAL;
		}
	}

	/* add queue channel */
	if (mode == TC_MQPRIO_MODE_CHANNEL) {
		if (!(vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ADQ)) {
			dev_err(&adapter->pdev->dev, "ADq not supported\n");
			return -EOPNOTSUPP;
		}
		if (adapter->ch_config.state != __IAVF_TC_INVALID) {
			dev_err(&adapter->pdev->dev, "TC configuration already exists\n");
			return -EINVAL;
		}

		ret = iavf_validate_ch_config(adapter, mqprio_qopt);
		if (ret)
			return ret;
		/* Return if same TC config is requested */
		if (adapter->num_tc == num_tc)
			return 0;
		adapter->num_tc = num_tc;

		for (i = 0; i < IAVF_MAX_TRAFFIC_CLASS; i++) {
			if (i < num_tc) {
				adapter->ch_config.ch_info[i].count =
					mqprio_qopt->qopt.count[i];
				adapter->ch_config.ch_info[i].offset =
					mqprio_qopt->qopt.offset[i];
				total_qps += mqprio_qopt->qopt.count[i];
				max_tx_rate = mqprio_qopt->max_rate[i];
				/* convert to Mbps */
				max_tx_rate = div_u64(max_tx_rate,
						      IAVF_MBPS_DIVISOR);
				adapter->ch_config.ch_info[i].max_tx_rate =
					max_tx_rate;
			} else {
				adapter->ch_config.ch_info[i].count = 1;
				adapter->ch_config.ch_info[i].offset = 0;
			}
		}
		adapter->ch_config.total_qps = total_qps;
		netif_tx_stop_all_queues(netdev);
		netif_tx_disable(netdev);
		adapter->aq_required |= IAVF_FLAG_AQ_ENABLE_CHANNELS;
		netdev_reset_tc(netdev);
		/* Report the tc mapping up the stack */
		netdev_set_num_tc(adapter->netdev, num_tc);
		for (i = 0; i < IAVF_MAX_TRAFFIC_CLASS; i++) {
			u16 qcount = mqprio_qopt->qopt.count[i];
			u16 qoffset = mqprio_qopt->qopt.offset[i];

			if (i < num_tc)
				netdev_set_tc_queue(netdev, netdev_tc++, qcount,
						    qoffset);
		}
	}
exit:
	return ret;
}

/**
 * iavf_parse_cls_flower - Parse tc flower filters provided by kernel
 * @adapter: board private structure
 * @f: pointer to struct tc_cls_flower_offload
 * @filter: pointer to cloud filter structure
 */
static int iavf_parse_cls_flower(struct iavf_adapter *adapter,
				 struct tc_cls_flower_offload *f,
				 struct iavf_cloud_filter *filter)
{
	struct flow_rule *rule = tc_cls_flower_offload_flow_rule(f);
	struct flow_dissector *dissector = rule->match.dissector;
	struct virtchnl_filter *cf = &filter->f;
	u16 n_proto_mask = 0;
	u16 n_proto_key = 0;
	u8 field_flags = 0;
	u16 addr_type = 0;
	u16 n_proto = 0;
	int i = 0;

	if (dissector->used_keys &
	    ~(BIT(FLOW_DISSECTOR_KEY_CONTROL) |
	      BIT(FLOW_DISSECTOR_KEY_BASIC) |
	      BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
	      BIT(FLOW_DISSECTOR_KEY_VLAN) |
	      BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
	      BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
#ifdef HAVE_TC_FLOWER_ENC
	      BIT(FLOW_DISSECTOR_KEY_ENC_KEYID) |
#endif /* HAVE_TC_FLOWER_ENC */
	      BIT(FLOW_DISSECTOR_KEY_PORTS))) {
		dev_err(&adapter->pdev->dev, "Unsupported key used: 0x%x\n",
			dissector->used_keys);
		return -EOPNOTSUPP;
	}

#ifdef HAVE_TC_FLOWER_ENC
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_KEYID)) {
		struct flow_match_enc_keyid match;

		flow_rule_match_enc_keyid(rule, &match);

		if (match.mask->keyid != 0)
			field_flags |= IAVF_CLOUD_FIELD_TEN_ID;
	}
#endif /* HAVE_TC_FLOWER_ENC */

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);

		n_proto_key = ntohs(match.key->n_proto);
		n_proto_mask = ntohs(match.mask->n_proto);

		if (n_proto_key == ETH_P_ALL) {
			n_proto_key = 0;
			n_proto_mask = 0;
		}
		n_proto = n_proto_key & n_proto_mask;
		if (n_proto != ETH_P_IP && n_proto != ETH_P_IPV6)
			return -EINVAL;
		if (n_proto == ETH_P_IPV6) {
			/* specify flow type as TCP IPv6 */
			cf->flow_type = VIRTCHNL_TCP_V6_FLOW;
			filter->f.flow_type = VIRTCHNL_TCP_V6_FLOW;
		}

		if (match.key->ip_proto != IPPROTO_TCP) {
			dev_info(&adapter->pdev->dev, "Only TCP transport is supported\n");
			return -EINVAL;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);

		/* use is_broadcast and is_zero to check for all 0xf or 0 */
		if (!is_zero_ether_addr(match.mask->dst)) {
			if (is_broadcast_ether_addr(match.mask->dst)) {
				field_flags |= IAVF_CLOUD_FIELD_OMAC;
			} else {
				dev_err(&adapter->pdev->dev, "Bad ether dest mask %pM\n",
					match.mask->dst);
				return IAVF_ERR_CONFIG;
			}
		}

		if (!is_zero_ether_addr(match.mask->src)) {
			if (is_broadcast_ether_addr(match.mask->src)) {
				field_flags |= IAVF_CLOUD_FIELD_IMAC;
			} else {
				dev_err(&adapter->pdev->dev, "Bad ether src mask %pM\n",
					match.mask->src);
				return IAVF_ERR_CONFIG;
			}
		}

		if (!is_zero_ether_addr(match.key->dst))
			if (is_valid_ether_addr(match.key->dst) ||
			    is_multicast_ether_addr(match.key->dst)) {
				/* set the mask if a valid dst_mac address */
				for (i = 0; i < ETH_ALEN; i++)
					cf->mask.tcp_spec.dst_mac[i] |= 0xff;
				ether_addr_copy(cf->data.tcp_spec.dst_mac,
						match.key->dst);
			}

		if (!is_zero_ether_addr(match.key->src))
			if (is_valid_ether_addr(match.key->src) ||
			    is_multicast_ether_addr(match.key->src)) {
				/* set the mask if a valid src_mac address */
				for (i = 0; i < ETH_ALEN; i++)
					cf->mask.tcp_spec.src_mac[i] |= 0xff;
				ether_addr_copy(cf->data.tcp_spec.src_mac,
						match.key->src);
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_vlan(rule, &match);

		if (match.mask->vlan_id) {
			if (match.mask->vlan_id == VLAN_VID_MASK) {
				field_flags |= IAVF_CLOUD_FIELD_IVLAN;
			} else {
				dev_err(&adapter->pdev->dev, "Bad vlan mask %u\n",
					match.mask->vlan_id);
				return IAVF_ERR_CONFIG;
			}
		}
		cf->mask.tcp_spec.vlan_id |= cpu_to_be16(0xffff);
		cf->data.tcp_spec.vlan_id = cpu_to_be16(match.key->vlan_id);
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);

		if (match.mask->dst) {
			if (match.mask->dst == cpu_to_be32(0xffffffff)) {
				field_flags |= IAVF_CLOUD_FIELD_IIP;
			} else {
				dev_err(&adapter->pdev->dev, "Bad ip dst mask 0x%08x\n",
					be32_to_cpu(match.mask->dst));
				return IAVF_ERR_CONFIG;
			}
		}

		if (match.mask->src) {
			if (match.mask->src == cpu_to_be32(0xffffffff)) {
				field_flags |= IAVF_CLOUD_FIELD_IIP;
			} else {
				dev_err(&adapter->pdev->dev, "Bad ip src mask 0x%08x\n",
					be32_to_cpu(match.mask->dst));
				return IAVF_ERR_CONFIG;
			}
		}

		if (field_flags & IAVF_CLOUD_FIELD_TEN_ID) {
			dev_info(&adapter->pdev->dev, "Tenant id not allowed for ip filter\n");
			return IAVF_ERR_CONFIG;
		}
		if (match.key->dst) {
			cf->mask.tcp_spec.dst_ip[0] |= cpu_to_be32(0xffffffff);
			cf->data.tcp_spec.dst_ip[0] = match.key->dst;
		}
		if (match.key->src) {
			cf->mask.tcp_spec.src_ip[0] |= cpu_to_be32(0xffffffff);
			cf->data.tcp_spec.src_ip[0] = match.key->src;
		}
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_ipv6_addrs(rule, &match);

		/* validate mask, make sure it is not IPV6_ADDR_ANY */
		if (ipv6_addr_any(&match.mask->dst)) {
			dev_err(&adapter->pdev->dev, "Bad ipv6 dst mask 0x%02x\n",
				IPV6_ADDR_ANY);
			return IAVF_ERR_CONFIG;
		}

		/* src and dest IPv6 address should not be LOOPBACK
		 * (0:0:0:0:0:0:0:1) which can be represented as ::1
		 */
		if (ipv6_addr_loopback(&match.key->dst) ||
		    ipv6_addr_loopback(&match.key->src)) {
			dev_err(&adapter->pdev->dev,
				"ipv6 addr should not be loopback\n");
			return IAVF_ERR_CONFIG;
		}
		if (!ipv6_addr_any(&match.mask->dst) ||
		    !ipv6_addr_any(&match.mask->src))
			field_flags |= IAVF_CLOUD_FIELD_IIP;

		for (i = 0; i < 4; i++)
			cf->mask.tcp_spec.dst_ip[i] |= cpu_to_be32(0xffffffff);
		memcpy(&cf->data.tcp_spec.dst_ip, &match.key->dst.s6_addr32,
		       sizeof(cf->data.tcp_spec.dst_ip));
		for (i = 0; i < 4; i++)
			cf->mask.tcp_spec.src_ip[i] |= cpu_to_be32(0xffffffff);
		memcpy(&cf->data.tcp_spec.src_ip, &match.key->src.s6_addr32,
		       sizeof(cf->data.tcp_spec.src_ip));
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);

		if (match.mask->src) {
			if (match.mask->src == cpu_to_be16(0xffff)) {
				field_flags |= IAVF_CLOUD_FIELD_IIP;
			} else {
				dev_err(&adapter->pdev->dev, "Bad src port mask %u\n",
					be16_to_cpu(match.mask->src));
				return IAVF_ERR_CONFIG;
			}
		}

		if (match.mask->dst) {
			if (match.mask->dst == cpu_to_be16(0xffff)) {
				field_flags |= IAVF_CLOUD_FIELD_IIP;
			} else {
				dev_err(&adapter->pdev->dev, "Bad dst port mask %u\n",
					be16_to_cpu(match.mask->dst));
				return IAVF_ERR_CONFIG;
			}
		}
		if (match.key->dst) {
			cf->mask.tcp_spec.dst_port |= cpu_to_be16(0xffff);
			cf->data.tcp_spec.dst_port = match.key->dst;
		}

		if (match.key->src) {
			cf->mask.tcp_spec.src_port |= cpu_to_be16(0xffff);
			cf->data.tcp_spec.src_port = match.key->dst;
		}
	}
	cf->field_flags = field_flags;

	return 0;
}

/**
 * iavf_handle_tclass - Forward to a traffic class on the device
 * @adapter: board private structure
 * @tc: traffic class index on the device
 * @filter: pointer to cloud filter structure
 */
static int iavf_handle_tclass(struct iavf_adapter *adapter, u32 tc,
			      struct iavf_cloud_filter *filter)
{
	if (tc == 0)
		return 0;
	if (tc < adapter->num_tc) {
		if (!filter->f.data.tcp_spec.dst_port) {
			dev_err(&adapter->pdev->dev,
				"Specify destination port to redirect to traffic class other than TC0\n");
			return -EINVAL;
		}
	}
	/* redirect to a traffic class on the same device */
	filter->f.action = VIRTCHNL_ACTION_TC_REDIRECT;
	filter->f.action_meta = tc;
	return 0;
}

/**
 * iavf_configure_clsflower - Add tc flower filters
 * @adapter: board private structure
 * @cls_flower: Pointer to struct tc_cls_flower_offload
 */
static int iavf_configure_clsflower(struct iavf_adapter *adapter,
				    struct tc_cls_flower_offload *cls_flower)
{
	int tc = tc_classid_to_hwtc(adapter->netdev, cls_flower->classid);
	struct iavf_cloud_filter *filter = NULL;
	int err = -EINVAL, count = 50;

	if (tc < 0) {
		dev_err(&adapter->pdev->dev, "Invalid traffic class\n");
		return -EINVAL;
	}

	filter = kzalloc(sizeof(*filter), GFP_KERNEL);
	if (!filter)
		return -ENOMEM;

	while (test_and_set_bit(__IAVF_IN_CRITICAL_TASK,
				&adapter->crit_section)) {
		if (--count == 0) {
			kfree(filter);
			return err;
		}
		udelay(1);
	}
	filter->cookie = cls_flower->cookie;

	/* set the mask to all zeroes to begin with */
	memset(&filter->f.mask.tcp_spec, 0, sizeof(struct virtchnl_l4_spec));
	/* start out with flow type and eth type IPv4 to begin with */
	filter->f.flow_type = VIRTCHNL_TCP_V4_FLOW;
	err = iavf_parse_cls_flower(adapter, cls_flower, filter);
	if (err < 0)
		goto err;

	err = iavf_handle_tclass(adapter, tc, filter);
	if (err < 0)
		goto err;

	/* add filter to the list */
	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_add_tail(&filter->list, &adapter->cloud_filter_list);
	adapter->num_cloud_filters++;
	filter->add = true;
	adapter->aq_required |= IAVF_FLAG_AQ_ADD_CLOUD_FILTER;
	spin_unlock_bh(&adapter->cloud_filter_list_lock);
err:
	if (err)
		kfree(filter);
	clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
	return err;
}

/* iavf_find_cf - Find the cloud filter in the list
 * @adapter: Board private structure
 * @cookie: filter specific cookie
 *
 * Returns ptr to the filter object or NULL. Must be called while holding the
 * cloud_filter_list_lock.
 */
static struct iavf_cloud_filter *iavf_find_cf(struct iavf_adapter *adapter,
					      unsigned long *cookie)
{
	struct iavf_cloud_filter *filter = NULL;

	if (!cookie)
		return NULL;

	list_for_each_entry(filter, &adapter->cloud_filter_list, list) {
		if (!memcmp(cookie, &filter->cookie, sizeof(filter->cookie)))
			return filter;
	}
	return NULL;
}

/**
 * iavf_delete_clsflower - Remove tc flower filters
 * @adapter: board private structure
 * @cls_flower: Pointer to struct tc_cls_flower_offload
 */
static int iavf_delete_clsflower(struct iavf_adapter *adapter,
				 struct tc_cls_flower_offload *cls_flower)
{
	struct iavf_cloud_filter *filter = NULL;
	int err = 0;

	spin_lock_bh(&adapter->cloud_filter_list_lock);
	filter = iavf_find_cf(adapter, &cls_flower->cookie);
	if (filter) {
		filter->del = true;
		adapter->aq_required |= IAVF_FLAG_AQ_DEL_CLOUD_FILTER;
	} else {
		err = -EINVAL;
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);

	return err;
}

/**
 * iavf_setup_tc_cls_flower - flower classifier offloads
 * @adapter: board private structure
 * @cls_flower: pointer to struct tc_cls_flower_offload
 */
static int iavf_setup_tc_cls_flower(struct iavf_adapter *adapter,
				    struct tc_cls_flower_offload *cls_flower)
{
	if (cls_flower->common.chain_index)
		return -EOPNOTSUPP;

	switch (cls_flower->command) {
	case TC_CLSFLOWER_REPLACE:
		return iavf_configure_clsflower(adapter, cls_flower);
	case TC_CLSFLOWER_DESTROY:
		return iavf_delete_clsflower(adapter, cls_flower);
	case TC_CLSFLOWER_STATS:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}

/**
 * iavf_setup_tc_block_cb - block callback for tc
 * @type: type of offload
 * @type_data: offload data
 * @cb_priv:
 *
 * This function is the block callback for traffic classes
 **/
static int iavf_setup_tc_block_cb(enum tc_setup_type type, void *type_data,
				  void *cb_priv)
{
	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return iavf_setup_tc_cls_flower(cb_priv, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * iavf_setup_tc_block - register callbacks for tc
 * @dev: network interface device structure
 * @f: tc offload data
 *
 * This function registers block callbacks for tc
 * offloads
 **/
static int iavf_setup_tc_block(struct net_device *dev,
			       struct tc_block_offload *f)
{
	struct iavf_adapter *adapter = netdev_priv(dev);

	if (f->binder_type != TCF_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	switch (f->command) {
	case TC_BLOCK_BIND:
#ifdef HAVE_TCF_BLOCK_CB_REGISTER_EXTACK
		return tcf_block_cb_register(f->block, iavf_setup_tc_block_cb,
					     adapter, adapter, f->extack);
#else
		return tcf_block_cb_register(f->block, iavf_setup_tc_block_cb,
					     adapter, adapter);
#endif
	case TC_BLOCK_UNBIND:
		tcf_block_cb_unregister(f->block, iavf_setup_tc_block_cb,
					adapter);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * iavf_setup_tc - configure multiple traffic classes
 * @dev: network interface device structure
 * @type: type of offload
 * @type_data: tc offload data
 *
 * This function is the callback to ndo_setup_tc in the
 * netdev_ops.
 *
 * Returns 0 on success
 **/
static int iavf_setup_tc(struct net_device *dev, enum tc_setup_type type,
			 void *type_data)
{
	switch (type) {
	case TC_SETUP_QDISC_MQPRIO:
		return __iavf_setup_tc(dev, type_data);
	case TC_SETUP_BLOCK:
		return iavf_setup_tc_block(dev, type_data);
	default:
		return -EOPNOTSUPP;
	}

	return __iavf_setup_tc(dev, type_data);
}
#endif /* __TC_MQPRIO_MODE_MAX */
#endif /* HAVE_NDO_SETUP_TC_REMOVE_TC_TO_NETDEV */
#endif /* HAVE_SETUP_TC */

/**
 * iavf_open - Called when a network interface is made active
 * @netdev: network interface device structure
 *
 * Returns 0 on success, negative value on failure
 *
 * The open entry point is called when a network interface is made
 * active by the system (IFF_UP).  At this point all resources needed
 * for transmit and receive operations are allocated, the interrupt
 * handler is registered with the OS, the watchdog is started,
 * and the stack is notified that the interface is ready.
 **/
static int iavf_open(struct net_device *netdev)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	int err;

	while (test_and_set_bit(__IAVF_IN_CRITICAL_TASK,
				&adapter->crit_section))
		usleep_range(500, 1000);

	if (adapter->flags & IAVF_FLAG_PF_COMMS_FAILED) {
		dev_err(&adapter->pdev->dev, "Unable to open device due to PF driver failure.\n");
		err = -EIO;
		goto err_unlock;
	}

	if (adapter->state != __IAVF_DOWN) {
		err = -EBUSY;
		goto err_unlock;
	}

	/* allocate transmit descriptors */
	err = iavf_setup_all_tx_resources(adapter);
	if (err)
		goto err_setup_tx;

	/* allocate receive descriptors */
	err = iavf_setup_all_rx_resources(adapter);
	if (err)
		goto err_setup_rx;

	/* clear any pending interrupts, may auto mask */
	err = iavf_request_traffic_irqs(adapter, netdev->name);
	if (err)
		goto err_req_irq;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	iavf_add_filter(adapter, adapter->hw.mac.addr);

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	/* Restore VLAN and Cloud filters that were removed with IFF_DOWN */
	iavf_restore_filters(adapter);

	iavf_configure(adapter);

	iavf_up_complete(adapter);

	iavf_irq_enable(adapter, true);

	clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);

	return 0;

err_req_irq:
	iavf_down(adapter);
	iavf_free_traffic_irqs(adapter);
err_setup_rx:
	iavf_free_all_rx_resources(adapter);
err_setup_tx:
	iavf_free_all_tx_resources(adapter);
err_unlock:
	clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);

	return err;
}

/**
 * iavf_close - Disables a network interface
 * @netdev: network interface device structure
 *
 * Returns 0, this is not allowed to fail
 *
 * The close entry point is called when an interface is de-activated
 * by the OS.  The hardware is still under the drivers control, but
 * needs to be disabled. All IRQs except vector 0 (reserved for admin queue)
 * are freed, along with all transmit and receive resources.
 **/
static int iavf_close(struct net_device *netdev)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	int status;

	while (test_and_set_bit(__IAVF_IN_CRITICAL_TASK,
				&adapter->crit_section))
		usleep_range(500, 1000);

	if (adapter->state <= __IAVF_DOWN_PENDING) {
		clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);
		return 0;
	}

	set_bit(__IAVF_VSI_DOWN, adapter->vsi.state);
	if (CLIENT_ENABLED(adapter))
		adapter->flags |= IAVF_FLAG_CLIENT_NEEDS_CLOSE;

	iavf_down(adapter);
	iavf_change_state(adapter, __IAVF_DOWN_PENDING);
	iavf_free_traffic_irqs(adapter);

	clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);

	/* We explicitly don't free resources here because the hardware is
	 * still active and can DMA into memory. Resources are cleared in
	 * iavf_virtchnl_completion() after we get confirmation from the PF
	 * driver that the rings have been stopped.
	 *
	 * Also, we wait for state to transition to __IAVF_DOWN before
	 * returning. State change occurs in iavf_virtchnl_completion() after
	 * VF resources are released (which occurs after PF driver processes and
	 * responds to admin queue commands).
	 */

	status = wait_event_timeout(adapter->down_waitqueue,
				    adapter->state == __IAVF_DOWN,
				    msecs_to_jiffies(500));
	if (!status)
		netdev_warn(netdev, "Device resources not yet released\n");
	return 0;
}

/**
 * iavf_get_stats - Get System Network Statistics
 * @netdev: network interface device structure
 *
 * Returns the address of the device statistics structure.
 * The statistics are actually updated from the watchdog task.
 **/
static struct net_device_stats *iavf_get_stats(struct net_device *netdev)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	/* only return the current stats */
	return &adapter->net_stats;
}

/**
 * iavf_change_mtu - Change the Maximum Transfer Unit
 * @netdev: network interface device structure
 * @new_mtu: new value for maximum frame size
 *
 * Returns 0 on success, negative on failure
 **/
static int iavf_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct iavf_adapter *adapter = netdev_priv(netdev);
	int max_frame = new_mtu + IAVF_PACKET_HDR_PAD;

	if ((new_mtu < 68) || (max_frame > IAVF_MAX_RXBUFFER))
		return -EINVAL;

#ifndef HAVE_NDO_FEATURES_CHECK
	/* MTU < 576 causes problems with TSO */
	if (new_mtu < 576) {
		netdev->features &= ~NETIF_F_TSO;
		netdev->features &= ~NETIF_F_TSO6;
#ifdef HAVE_NDO_SET_FEATURES
	} else {
#ifndef HAVE_RHEL6_NET_DEVICE_OPS_EXT
		if (netdev->wanted_features & NETIF_F_TSO)
			netdev->features |= NETIF_F_TSO;
		if (netdev->wanted_features & NETIF_F_TSO6)
			netdev->features |= NETIF_F_TSO6;
#else
		if (netdev_extended(netdev)->wanted_features & NETIF_F_TSO)
			netdev->features |= NETIF_F_TSO;
		if (netdev_extended(netdev)->wanted_features & NETIF_F_TSO6)
			netdev->features |= NETIF_F_TSO6;
#endif /* HAVE_RHEL6_NET_DEVICE_OPS_EXT */
#endif /* HAVE_NDO_SET_FEATURES */
	}
#endif /* !HAVE_NDO_FEATURES_CHECK */
	netdev_info(netdev, "changing MTU from %d to %d\n",
		    netdev->mtu, new_mtu);
	netdev->mtu = new_mtu;
	iavf_schedule_reset(adapter);

	return 0;
}

/**
 * iavf_set_features - set the netdev feature flags
 * @netdev: ptr to the netdev being adjusted
 * @features: the feature set that the stack is suggesting
 * Note: expects to be called while under rtnl_lock()
 **/
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
static int iavf_set_features(struct net_device *netdev, u32 features)
#else
static int iavf_set_features(struct net_device *netdev,
			     netdev_features_t features)
#endif
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	/* Don't allow changing VLAN_RX flag when adapter is not capable
	 * of VLAN offload
	 */
	if (!VLAN_ALLOWED(adapter)) {
#ifdef NETIF_F_HW_VLAN_CTAG_RX
		if ((netdev->features ^ features) & NETIF_F_HW_VLAN_CTAG_RX)
#else
		if ((netdev->features ^ features) & NETIF_F_HW_VLAN_RX)
#endif
			return -EINVAL;
#ifdef NETIF_F_HW_VLAN_CTAG_RX
	} else if ((netdev->features ^ features) & NETIF_F_HW_VLAN_CTAG_RX) {
		if (features & NETIF_F_HW_VLAN_CTAG_RX)
#else
	} else if ((netdev->features ^ features) & NETIF_F_HW_VLAN_RX) {
		if (features & NETIF_F_HW_VLAN_RX)
#endif
			adapter->aq_required |=
				IAVF_FLAG_AQ_ENABLE_VLAN_STRIPPING;
		else
			adapter->aq_required |=
				IAVF_FLAG_AQ_DISABLE_VLAN_STRIPPING;
	}

	return 0;
}

#ifdef HAVE_NDO_FEATURES_CHECK
/**
 * iavf_features_check - Validate encapsulated packet conforms to limits
 * @skb: skb buff
 * @dev: This physical port's netdev
 * @features: Offload features that the stack believes apply
 **/
static netdev_features_t iavf_features_check(struct sk_buff *skb,
					     struct net_device *dev,
					     netdev_features_t features)
{
	size_t len;

	/* No point in doing any of this if neither checksum nor GSO are
	 * being requested for this frame.  We can rule out both by just
	 * checking for CHECKSUM_PARTIAL
	 */
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return features;

	/* We cannot support GSO if the MSS is going to be less than
	 * 64 bytes.  If it is then we need to drop support for GSO.
	 */
	if (skb_is_gso(skb) && (skb_shinfo(skb)->gso_size < 64))
		features &= ~NETIF_F_GSO_MASK;

	/* MACLEN can support at most 63 words */
	len = skb_network_header(skb) - skb->data;
	if (len & ~(63 * 2))
		goto out_err;

	/* IPLEN and EIPLEN can support at most 127 dwords */
	len = skb_transport_header(skb) - skb_network_header(skb);
	if (len & ~(127 * 4))
		goto out_err;

	if (skb->encapsulation) {
		/* L4TUNLEN can support 127 words */
		len = skb_inner_network_header(skb) - skb_transport_header(skb);
		if (len & ~(127 * 2))
			goto out_err;

		/* IPLEN can support at most 127 dwords */
		len = skb_inner_transport_header(skb) -
		      skb_inner_network_header(skb);
		if (len & ~(127 * 4))
			goto out_err;
	}

	/* No need to validate L4LEN as TCP is the only protocol with a
	 * a flexible value and we support all possible values supported
	 * by TCP, which is at most 15 dwords
	 */

	return features;
out_err:
	return features & ~(NETIF_F_CSUM_MASK | NETIF_F_GSO_MASK);
}

#endif /* HAVE_NDO_FEATURES_CHECK */
#ifdef HAVE_NDO_SET_FEATURES
/**
 * iavf_fix_features - fix up the netdev feature bits
 * @netdev: our net device
 * @features: desired feature bits
 *
 * Returns fixed-up features bits
 **/
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
static u32 iavf_fix_features(struct net_device *netdev, u32 features)
#else
static netdev_features_t iavf_fix_features(struct net_device *netdev,
					   netdev_features_t features)
#endif
{
	struct iavf_adapter *adapter = netdev_priv(netdev);

	if (adapter->vf_res &&
	    !(adapter->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_VLAN))
#ifdef NETIF_F_HW_VLAN_CTAG_RX
		features &= ~(NETIF_F_HW_VLAN_CTAG_TX |
			      NETIF_F_HW_VLAN_CTAG_RX |
			      NETIF_F_HW_VLAN_CTAG_FILTER);
#else
		features &= ~(NETIF_F_HW_VLAN_TX |
			      NETIF_F_HW_VLAN_RX |
			      NETIF_F_HW_VLAN_FILTER);
#endif

	return features;
}

#endif /* HAVE_NDO_SET_FEATURES */
static const struct net_device_ops iavf_netdev_ops = {
#ifdef HAVE_RHEL7_NET_DEVICE_OPS_EXT
/* RHEL7 requires this to be defined to enable extended ops.  RHEL7 uses the
 * function get_ndo_ext to retrieve offsets for extended fields from with the
 * net_device_ops struct and ndo_size is checked to determine whether or not
 * the offset is valid.
 */
	.ndo_size		= sizeof(const struct net_device_ops),
#endif
	.ndo_open		= iavf_open,
	.ndo_stop		= iavf_close,
	.ndo_start_xmit		= iavf_lan_xmit_frame,
	.ndo_get_stats		= iavf_get_stats,
	.ndo_set_rx_mode	= iavf_set_rx_mode,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= iavf_set_mac,
#ifdef HAVE_RHEL7_EXTENDED_MIN_MAX_MTU
	.extended.ndo_change_mtu = iavf_change_mtu,
#else
	.ndo_change_mtu		= iavf_change_mtu,
#endif /* HAVE_RHEL7_EXTENDED_MIN_MAX_MTU */
	.ndo_tx_timeout		= iavf_tx_timeout,
#ifdef HAVE_VLAN_RX_REGISTER
	.ndo_vlan_rx_register	= iavf_vlan_rx_register,
#endif
	.ndo_vlan_rx_add_vid	= iavf_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= iavf_vlan_rx_kill_vid,
#ifdef HAVE_SETUP_TC
#ifdef HAVE_NDO_SETUP_TC_REMOVE_TC_TO_NETDEV
#ifdef __TC_MQPRIO_MODE_MAX
#ifdef HAVE_RHEL7_NETDEV_OPS_EXT_NDO_SETUP_TC
	.extended.ndo_setup_tc_rh = iavf_setup_tc,
#else
	.ndo_setup_tc		= iavf_setup_tc,
#endif /* HAVE_RHEL7_NETDEV_OPS_EXT_NDO_SETUP_TC */
#endif /* __TC_MQPRIO_MODE_MAX */
#endif /* HAVE_NDO_SETUP_TC_REMOVE_TC_TO_NETDEV */
#endif /* HAVE_SETUP_TC */
#ifdef HAVE_NDO_FEATURES_CHECK
	.ndo_features_check     = iavf_features_check,
#endif /* HAVE_NDO_FEATURES_CHECK */
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
};

/* RHEL6 keeps these operations in a separate structure */
static const struct net_device_ops_ext iavf_netdev_ops_ext = {
	.size			= sizeof(struct net_device_ops_ext),
#endif /* HAVE_RHEL6_NET_DEVICE_OPS_EXT */
#ifdef HAVE_NDO_SET_FEATURES
	.ndo_fix_features	= iavf_fix_features,
	.ndo_set_features	= iavf_set_features,
#endif /* HAVE_NDO_SET_FEATURES */
};

/**
 * iavf_check_reset_complete - check that VF reset is complete
 * @hw: pointer to hw struct
 *
 * Returns 0 if device is ready to use, or -EBUSY if it's in reset.
 **/
static int iavf_check_reset_complete(struct iavf_hw *hw)
{
	u32 rstat;
	int i;

	for (i = 0; i < IAVF_RESET_WAIT_COUNT; i++) {
		rstat = rd32(hw, IAVF_VFGEN_RSTAT) &
			     IAVF_VFGEN_RSTAT_VFR_STATE_MASK;
		if ((rstat == VIRTCHNL_VFR_VFACTIVE) ||
		    (rstat == VIRTCHNL_VFR_COMPLETED))
			return 0;

		usleep_range(10, 20);
	}
	return -EBUSY;
}

/**
 * iavf_process_config - Process the config information we got from the PF
 * @adapter: board private structure
 *
 * Verify that we have a valid config struct, and set up our netdev features
 * and our VSI struct.
 **/
int iavf_process_config(struct iavf_adapter *adapter)
{
	struct virtchnl_vf_resource *vfres = adapter->vf_res;
	int i, num_req_queues = adapter->num_req_queues;
	struct net_device *netdev = adapter->netdev;
	struct iavf_vsi *vsi = &adapter->vsi;
	netdev_features_t hw_enc_features;
	netdev_features_t hw_features;

	/* got VF config message back from PF, now we can parse it */
	for (i = 0; i < vfres->num_vsis; i++) {
		if (vfres->vsi_res[i].vsi_type == VIRTCHNL_VSI_SRIOV)
			adapter->vsi_res = &vfres->vsi_res[i];
	}
	if (!adapter->vsi_res) {
		dev_err(&adapter->pdev->dev, "No LAN VSI found\n");
		return -ENODEV;
	}

	if (num_req_queues &&
	    num_req_queues != adapter->vsi_res->num_queue_pairs) {
		/* Problem.  The PF gave us fewer queues than what we had
		 * negotiated in our request.  Need a reset to see if we can't
		 * get back to a working state.
		 */
		dev_err(&adapter->pdev->dev,
			"Requested %d queues, but PF only gave us %d.\n",
			num_req_queues,
			adapter->vsi_res->num_queue_pairs);
		adapter->flags |= IAVF_FLAG_REINIT_ITR_NEEDED;
		adapter->num_req_queues = adapter->vsi_res->num_queue_pairs;
		iavf_schedule_reset(adapter);
		return -ENODEV;
	}
	adapter->num_req_queues = 0;

	hw_enc_features = NETIF_F_SG			|
			  NETIF_F_IP_CSUM		|
#ifdef NETIF_F_IPV6_CSUM
			  NETIF_F_IPV6_CSUM		|
#endif
			  NETIF_F_HIGHDMA		|
#ifdef NETIF_F_SOFT_FEATURES
			  NETIF_F_SOFT_FEATURES	|
#endif
			  NETIF_F_TSO			|
			  NETIF_F_TSO_ECN		|
			  NETIF_F_TSO6			|
			  NETIF_F_SCTP_CRC		|
#ifdef NETIF_F_RXHASH
			  NETIF_F_RXHASH		|
#endif
#ifdef HAVE_NDO_SET_FEATURES
			  NETIF_F_RXCSUM		|
#endif
			  0;

#ifdef HAVE_ENCAP_CSUM_OFFLOAD
	/* advertise to stack only if offloads for encapsulated packets is
	 * supported
	 */
	if (vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ENCAP) {
#ifdef HAVE_ENCAP_TSO_OFFLOAD
		hw_enc_features |= NETIF_F_GSO_UDP_TUNNEL	|
#ifdef HAVE_GRE_ENCAP_OFFLOAD
				   NETIF_F_GSO_GRE		|
#ifdef NETIF_F_GSO_PARTIAL
				   NETIF_F_GSO_GRE_CSUM		|
				   NETIF_F_GSO_PARTIAL		|
#endif
				   NETIF_F_GSO_UDP_TUNNEL_CSUM	|
#ifdef NETIF_F_GSO_IPXIP4
				   NETIF_F_GSO_IPXIP4		|
#ifdef NETIF_F_GSO_IPXIP6
				   NETIF_F_GSO_IPXIP6		|
#endif
#else /* NETIF_F_GSO_IPXIP4 */
#ifdef NETIF_F_GSO_IPIP
				   NETIF_F_GSO_IPIP		|
#endif
#ifdef NETIF_F_GSO_SIT
				   NETIF_F_GSO_SIT		|
#endif
#endif /* NETIF_F_GSO_IPXIP4 */
#endif /* NETIF_F_GRE_ENCAP_OFFLOAD */
				   0;

		if (!(vfres->vf_cap_flags &
		      VIRTCHNL_VF_OFFLOAD_ENCAP_CSUM))
#ifndef NETIF_F_GSO_PARTIAL
			hw_enc_features ^= NETIF_F_GSO_UDP_TUNNEL_CSUM;
#else
			netdev->gso_partial_features |=
				NETIF_F_GSO_UDP_TUNNEL_CSUM;

		netdev->gso_partial_features |= NETIF_F_GSO_GRE_CSUM;
		netdev->hw_enc_features |= NETIF_F_TSO_MANGLEID;
#endif /* !NETIF_F_GSO_PARTIAL */
#endif /* HAVE_ENCAP_TSO_OFFLOAD */
		netdev->hw_enc_features |= hw_enc_features;
	}
#endif /* HAVE_ENCAP_CSUM_OFFLOAD */

#ifdef HAVE_NETDEV_VLAN_FEATURES
	/* record features VLANs can make use of */
#ifdef NETIF_F_GSO_PARTIAL
	netdev->vlan_features |= hw_enc_features | NETIF_F_TSO_MANGLEID;
#else
	netdev->vlan_features |= hw_enc_features;
#endif
#endif

	/* Write features and hw_features separately to avoid polluting
	 * with, or dropping, features that are set when we registered.
	 */
	hw_features = hw_enc_features;

	/* Enable VLAN features if supported */
	if (vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_VLAN)
#ifdef NETIF_F_HW_VLAN_CTAG_RX
		hw_features |= (NETIF_F_HW_VLAN_CTAG_TX |
				NETIF_F_HW_VLAN_CTAG_RX);
#else
		hw_features |= (NETIF_F_HW_VLAN_TX |
				NETIF_F_HW_VLAN_RX);
#endif
#ifdef NETIF_F_HW_TC
	/* Enable cloud filter if ADQ is supported */
	if (vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_ADQ)
		hw_features |= NETIF_F_HW_TC;
#endif

#ifdef HAVE_NDO_SET_FEATURES
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
	hw_features |= get_netdev_hw_features(netdev);
	set_netdev_hw_features(netdev, hw_features);
#else
	netdev->hw_features |= hw_features;
#endif
#endif /* HAVE_NDO_SET_FEATURES */

	netdev->features |= hw_features;

	if (vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_VLAN)
#ifdef NETIF_F_HW_VLAN_CTAG_RX
		netdev->features |= NETIF_F_HW_VLAN_CTAG_FILTER;
#else
		netdev->features |= NETIF_F_HW_VLAN_FILTER;
#endif

#ifdef IFF_UNICAST_FLT
	netdev->priv_flags |= IFF_UNICAST_FLT;

#endif
	/* Do not turn on offloads when they are requested to be turned off.
	 * TSO needs minimum 576 bytes to work correctly.
	 */
#ifndef HAVE_RHEL6_NET_DEVICE_OPS_EXT
	if (netdev->wanted_features) {
		if (!(netdev->wanted_features & NETIF_F_TSO) ||
		    netdev->mtu < 576)
			netdev->features &= ~NETIF_F_TSO;
		if (!(netdev->wanted_features & NETIF_F_TSO6) ||
		    netdev->mtu < 576)
			netdev->features &= ~NETIF_F_TSO6;
		if (!(netdev->wanted_features & NETIF_F_TSO_ECN))
			netdev->features &= ~NETIF_F_TSO_ECN;
		if (!(netdev->wanted_features & NETIF_F_GRO))
			netdev->features &= ~NETIF_F_GRO;
		if (!(netdev->wanted_features & NETIF_F_GSO))
			netdev->features &= ~NETIF_F_GSO;
#else
	if (netdev_extended(netdev)->wanted_features) {
		if (!(netdev_extended(netdev)->wanted_features &
		      NETIF_F_TSO) || netdev->mtu < 576)
			netdev->features &= ~NETIF_F_TSO;
		if (!(netdev_extended(netdev)->wanted_features &
		      NETIF_F_TSO6) || netdev->mtu < 576)
			netdev->features &= ~NETIF_F_TSO6;
		if (!(netdev_extended(netdev)->wanted_features &
		      NETIF_F_TSO_ECN))
			netdev->features &= ~NETIF_F_TSO_ECN;
		if (!(netdev_extended(netdev)->wanted_features & NETIF_F_GRO))
			netdev->features &= ~NETIF_F_GRO;
		if (!(netdev_extended(netdev)->wanted_features & NETIF_F_GSO))
			netdev->features &= ~NETIF_F_GSO;
#endif /* HAVE_RHEL6_NET_DEVICE_OPS_EXT */
	}

	adapter->vsi.id = adapter->vsi_res->vsi_id;

	adapter->vsi.back = adapter;
	adapter->vsi.base_vector = 1;
	adapter->vsi.work_limit = IAVF_DEFAULT_IRQ_WORK;
	vsi->netdev = adapter->netdev;
	vsi->qs_handle = adapter->vsi_res->qset_handle;
	if (vfres->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RSS_PF) {
		adapter->rss_key_size = vfres->rss_key_size;
		adapter->rss_lut_size = vfres->rss_lut_size;
	} else {
		adapter->rss_key_size = IAVF_HKEY_ARRAY_SIZE;
		adapter->rss_lut_size = IAVF_HLUT_ARRAY_SIZE;
	}

	return 0;
}

/**
 * iavf_shutdown - Shutdown the device in preparation for a reboot
 * @pdev: pci device structure
 **/
static void iavf_shutdown(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct iavf_adapter *adapter = netdev_priv(netdev);

	netif_device_detach(netdev);

	if (netif_running(netdev))
		iavf_close(netdev);

	/* Prevent the watchdog from running. */
	iavf_change_state(adapter, __IAVF_REMOVE);
	adapter->aq_required = 0;

#ifdef CONFIG_PM
	pci_save_state(pdev);

#endif
	pci_disable_device(pdev);
}

/**
 * iavf_probe - Device Initialization Routine
 * @pdev: PCI device information struct
 * @ent: entry in iavf_pci_tbl
 *
 * Returns 0 on success, negative on failure
 *
 * iavf_probe initializes an adapter identified by a pci_dev structure.
 * The OS initialization, configuring of the adapter private structure,
 * and a hardware reset occur.
 **/
#ifdef HAVE_CONFIG_HOTPLUG
static int __devinit iavf_probe(struct pci_dev *pdev,
				  const struct pci_device_id *ent)
#else
static int iavf_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
#endif
{
	struct net_device *netdev;
	struct iavf_adapter *adapter = NULL;
	struct iavf_hw *hw = NULL;
	int err;

	err = pci_enable_device(pdev);
	if (err)
		return err;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev,
				"DMA configuration failed: 0x%x\n", err);
			goto err_dma;
		}
	}

	err = pci_request_regions(pdev, iavf_driver_name);
	if (err) {
		dev_err(pci_dev_to_dev(pdev),
			"pci_request_regions failed 0x%x\n", err);
		goto err_pci_reg;
	}

	pci_enable_pcie_error_reporting(pdev);

	pci_set_master(pdev);

	netdev = alloc_etherdev_mq(sizeof(struct iavf_adapter),
				   IAVF_MAX_REQ_QUEUES);
	if (!netdev) {
		err = -ENOMEM;
		goto err_alloc_etherdev;
	}

	SET_NETDEV_DEV(netdev, &pdev->dev);

	pci_set_drvdata(pdev, netdev);
	adapter = netdev_priv(netdev);

	adapter->netdev = netdev;
	adapter->pdev = pdev;

	hw = &adapter->hw;
	hw->back = adapter;

	adapter->msg_enable = (1 << DEFAULT_DEBUG_LEVEL_SHIFT) - 1;
	iavf_change_state(adapter, __IAVF_STARTUP);

	/* Call save state here because it relies on the adapter struct. */
	pci_save_state(pdev);

	hw->hw_addr = ioremap(pci_resource_start(pdev, 0),
			      pci_resource_len(pdev, 0));
	if (!hw->hw_addr) {
		err = -EIO;
		goto err_ioremap;
	}
	hw->vendor_id = pdev->vendor;
	hw->device_id = pdev->device;
	pci_read_config_byte(pdev, PCI_REVISION_ID, &hw->revision_id);
	hw->subsystem_vendor_id = pdev->subsystem_vendor;
	hw->subsystem_device_id = pdev->subsystem_device;
	hw->bus.device = PCI_SLOT(pdev->devfn);
	hw->bus.func = PCI_FUNC(pdev->devfn);
	hw->bus.bus_id = pdev->bus->number;

	/* set up the spinlocks for the AQ, do this only once in probe
	 * and destroy them only once in remove
	 */
	iavf_init_spinlock_d(&hw->aq.asq_spinlock);
	iavf_init_spinlock_d(&hw->aq.arq_spinlock);

	spin_lock_init(&adapter->mac_vlan_list_lock);
	spin_lock_init(&adapter->cloud_filter_list_lock);

	INIT_LIST_HEAD(&adapter->mac_filter_list);
	INIT_LIST_HEAD(&adapter->vlan_filter_list);
	INIT_LIST_HEAD(&adapter->cloud_filter_list);

	INIT_WORK(&adapter->adminq_task, iavf_adminq_task);
	INIT_DELAYED_WORK(&adapter->watchdog_task, iavf_watchdog_task);
	INIT_DELAYED_WORK(&adapter->client_task, iavf_client_task);
	queue_delayed_work(iavf_wq, &adapter->watchdog_task,
			   msecs_to_jiffies(5 * (pdev->devfn & 0x07)));
	/* Setup the wait queue for indicating transition to down status */
	init_waitqueue_head(&adapter->down_waitqueue);

	return 0;

err_ioremap:
	free_netdev(netdev);
err_alloc_etherdev:
	pci_release_regions(pdev);
err_pci_reg:
err_dma:
	pci_disable_device(pdev);
	return err;
}

#ifdef CONFIG_PM
/**
 * iavf_suspend - Power management suspend routine
 * @pdev: PCI device information struct
 * @state: unused
 *
 * Called when the system (VM) is entering sleep/suspend.
 **/
static int iavf_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct iavf_adapter *adapter = netdev_priv(netdev);
	int retval = 0;

	netif_device_detach(netdev);

	while (test_and_set_bit(__IAVF_IN_CRITICAL_TASK,
				&adapter->crit_section))
		usleep_range(500, 1000);

	if (netif_running(netdev)) {
		rtnl_lock();
		iavf_down(adapter);
		rtnl_unlock();
	}
	iavf_free_misc_irq(adapter);
	iavf_reset_interrupt_capability(adapter);

	clear_bit(__IAVF_IN_CRITICAL_TASK, &adapter->crit_section);

	retval = pci_save_state(pdev);
	if (retval)
		return retval;

	pci_disable_device(pdev);

	return 0;
}

/**
 * iavf_resume - Power management resume routine
 * @pdev: PCI device information struct
 *
 * Called when the system (VM) is resumed from sleep/suspend.
 **/
static int iavf_resume(struct pci_dev *pdev)
{
	struct iavf_adapter *adapter = pci_get_drvdata(pdev);
	struct net_device *netdev = adapter->netdev;
	u32 err;

	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);
	/* pci_restore_state clears dev->state_saved so call
	 * pci_save_state to restore it.
	 */
	pci_save_state(pdev);

	err = pci_enable_device_mem(pdev);
	if (err) {
		dev_err(&pdev->dev, "Cannot enable PCI device from suspend.\n");
		return err;
	}
	pci_set_master(pdev);

	rtnl_lock();
	err = iavf_set_interrupt_capability(adapter);
	if (err) {
		dev_err(&pdev->dev, "Cannot enable MSI-X interrupts.\n");
		return err;
	}
	err = iavf_request_misc_irq(adapter);
	rtnl_unlock();
	if (err) {
		dev_err(&pdev->dev, "Cannot get interrupt vector.\n");
		return err;
	}

	iavf_schedule_reset(adapter);
	netif_device_attach(netdev);

	return err;
}

#endif /* CONFIG_PM */
/**
 * iavf_remove - Device Removal Routine
 * @pdev: PCI device information struct
 *
 * iavf_remove is called by the PCI subsystem to alert the driver
 * that it should release a PCI device.  The could be caused by a
 * Hot-Plug event, or because the driver is going to be removed from
 * memory.
 **/
#ifdef HAVE_CONFIG_HOTPLUG
static void __devexit iavf_remove(struct pci_dev *pdev)
#else
static void iavf_remove(struct pci_dev *pdev)
#endif
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct iavf_adapter *adapter = netdev_priv(netdev);
	struct iavf_vlan_filter *vlf, *vlftmp;
	struct iavf_cloud_filter *cf, *cftmp;
	struct iavf_mac_filter *f, *ftmp;
	struct iavf_hw *hw = &adapter->hw;
	int err;
	/* Indicate we are in remove and not to run reset_task */
	set_bit(__IAVF_IN_REMOVE_TASK, &adapter->crit_section);
	cancel_delayed_work_sync(&adapter->client_task);
	if (adapter->netdev_registered) {
		/* will call iavf_close if the device was open previously */
		unregister_netdev(netdev);
		adapter->netdev_registered = false;
		/* wait for device down */
		wait_event_timeout(adapter->down_waitqueue,
				   adapter->state == __IAVF_DOWN,
				   msecs_to_jiffies(200));
	}

	if (CLIENT_ALLOWED(adapter)) {
		err = iavf_lan_del_device(adapter);
		if (err)
			dev_warn(&pdev->dev, "Failed to delete client device: %d\n",
				 err);
	}

	/* Shut down all the garbage mashers on the detention level */
	iavf_change_state(adapter, __IAVF_REMOVE);
	adapter->aq_required = 0;
	adapter->flags &= ~IAVF_FLAG_REINIT_ITR_NEEDED;
	iavf_request_reset(adapter);
	msleep(50);
	/* If the FW isn't responding, kick it once, but only once. */
	if (!iavf_asq_done(hw)) {
		iavf_request_reset(adapter);
		msleep(50);
	}

	iavf_misc_irq_disable(adapter);
	cancel_work_sync(&adapter->adminq_task);

	cancel_delayed_work_sync(&adapter->watchdog_task);

	iavf_free_all_tx_resources(adapter);
	iavf_free_all_rx_resources(adapter);
	iavf_free_misc_irq(adapter);
	iavf_reset_interrupt_capability(adapter);
	iavf_free_q_vectors(adapter);
	iavf_free_rss(adapter);

	if (hw->aq.asq.count)
		iavf_shutdown_adminq(hw);

	/* destroy the locks only once, here */
	iavf_destroy_spinlock_d(&hw->aq.arq_spinlock);
	iavf_destroy_spinlock_d(&hw->aq.asq_spinlock);

	iounmap(hw->hw_addr);
	pci_release_regions(pdev);

	iavf_free_queues(adapter);
	kfree(adapter->vf_res);
	adapter->vf_res = NULL;

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	/* If we got removed before an up/down sequence, we've got a filter
	 * hanging out there that we need to get rid of.
	 */
	list_for_each_entry_safe(f, ftmp, &adapter->mac_filter_list, list) {
		list_del(&f->list);
		kfree(f);
	}
	list_for_each_entry_safe(vlf, vlftmp, &adapter->vlan_filter_list,
				 list) {
		list_del(&vlf->list);
		kfree(vlf);
	}

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	spin_lock_bh(&adapter->cloud_filter_list_lock);
	list_for_each_entry_safe(cf, cftmp, &adapter->cloud_filter_list, list) {
		list_del(&cf->list);
		kfree(cf);
	}
	spin_unlock_bh(&adapter->cloud_filter_list_lock);

	free_netdev(netdev);

	pci_disable_pcie_error_reporting(pdev);

	pci_disable_device(pdev);
}

static struct pci_driver iavf_driver = {
	.name     = iavf_driver_name,
	.id_table = iavf_pci_tbl,
	.probe    = iavf_probe,
#ifdef HAVE_CONFIG_HOTPLUG
	.remove   = __devexit_p(iavf_remove),
#else
	.remove   = iavf_remove,
#endif
#ifdef CONFIG_PM
	.suspend  = iavf_suspend,
	.resume   = iavf_resume,
#endif
	.shutdown = iavf_shutdown,
};

/**
 * iavf_init_module - Driver Registration Routine
 *
 * iavf_init_module is the first routine called when the driver is
 * loaded. All it does is register with the PCI subsystem.
 **/
static int __init iavf_init_module(void)
{
	int ret;

	pr_info("iavf: %s - version %s\n", iavf_driver_string,
		iavf_driver_version);

	pr_info("%s\n", iavf_copyright);

	iavf_wq = alloc_workqueue("%s", WQ_MEM_RECLAIM, 0,
				  iavf_driver_name);
	if (!iavf_wq) {
		pr_err("%s: Failed to create workqueue\n", iavf_driver_name);
		return -ENOMEM;
	}
	ret = pci_register_driver(&iavf_driver);
	return ret;
}

module_init(iavf_init_module);

/**
 * iavf_exit_module - Driver Exit Cleanup Routine
 *
 * iavf_exit_module is called just before the driver is removed
 * from memory.
 **/
static void __exit iavf_exit_module(void)
{
	pci_unregister_driver(&iavf_driver);
	destroy_workqueue(iavf_wq);
}

module_exit(iavf_exit_module);

/* iavf_main.c */
