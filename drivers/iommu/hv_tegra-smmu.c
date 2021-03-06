/*
 * Virtual IOMMU driver for SMMU on Tegra 12x series SoCs and later.
 *
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/dma-attrs.h>
#include "tegra-smmu.h"
#include "hv_tegra-smmu-lib.h"

static struct smmu_device *smmu_handle; /* assume only one smmu device */

/* Make sure not to call this with as lock held */
static struct smmu_as *smmu_as_alloc_hv(void)
{
	int i;
	unsigned long flags;
	struct smmu_as *as;
	struct smmu_device *smmu = smmu_handle;

	/* Look for a free AS with as comm channel lock held */
	for  (i = 0; i < smmu->num_as; i++) {
		int chan;
		spinlock_t *lock;

		as = &smmu->as[i];
		lock = &as->tegra_hv_comm_chan_lock;
		spin_lock_irqsave(lock, flags);
		/*
		 * No channel allocated is a indication
		 * this asid is not used
		 */
		if (as->tegra_hv_comm_chan == COMM_CHAN_UNASSIGNED) {
			chan = tegra_hv_smmu_comm_chan_alloc();
			if (chan >= 0) {
				as->tegra_hv_comm_chan = chan;
				spin_unlock_irqrestore(lock, flags);
				return as;
			} else {
				spin_unlock_irqrestore(lock, flags);
				break;
			}
		}
		spin_unlock_irqrestore(lock, flags);
	}

	if (i == smmu->num_as)
		dev_err(smmu->dev,  "no free AS\n");

	return ERR_PTR(-EINVAL);
}

static void smmu_as_free_hv(struct smmu_domain *dom,
				unsigned long as_alloc_bitmap)
{
	int idx;
	unsigned long flags;
	spinlock_t *lock;

	for_each_set_bit(idx, &as_alloc_bitmap, MAX_AS_PER_DEV) {
		lock = &dom->as[idx]->tegra_hv_comm_chan_lock;
		spin_lock_irqsave(lock, flags);
		tegra_hv_smmu_comm_chan_free(dom->as[idx]->tegra_hv_comm_chan);
		dom->as[idx]->tegra_hv_comm_chan = COMM_CHAN_UNASSIGNED;
		dom->as[idx] = NULL;
		spin_unlock_irqrestore(lock, flags);
	}
}

static int smmu_client_set_hwgrp_hv(struct smmu_client *c, u64 map, int on)
{
	struct smmu_domain *dom = c->domain;
	struct smmu_as *as = smmu_as_bitmap(dom);
	u32 i;
	int err = 0;

	WARN_ON(!on && map);
	if (on && !map)
		return -EINVAL;
	if (!on)
		map = c->swgids;

	/* TODO t210 supports multiple asid per hwgroup
	 * add that support
	 */
	for_each_set_bit(i, (unsigned long *)&map, HWGRP_COUNT) {
		if (on)
			err = libsmmu_attach_hwgrp(as->tegra_hv_comm_chan,
							i, as->asid);
		else if (list_empty(&c->list))
			/* turn off if this is the last */
			err = libsmmu_detach_hwgrp(as->tegra_hv_comm_chan, i);
		else
			return 0; /* leave if off but not the last */

	}

	c->swgids = map;
	return err;
}

static int __smmu_iommu_map_pfn_hv(struct smmu_as *as, dma_addr_t iova,
					unsigned long pfn, unsigned long prot)
{
	int attrs = as->pte_attr;

	if (dma_get_attr(DMA_ATTR_READ_ONLY, (struct dma_attrs *)prot))
		attrs &= ~_WRITABLE;
	else if (dma_get_attr(DMA_ATTR_WRITE_ONLY, (struct dma_attrs *)prot))
		attrs &= ~_READABLE;

	return libsmmu_map_page(as->tegra_hv_comm_chan, as->asid, iova,
					__pfn_to_phys(pfn), attrs);
}

static int __smmu_iommu_map_page_hv(struct smmu_as *as, dma_addr_t iova,
					phys_addr_t pa, unsigned long prot)
{
	unsigned long pfn = __phys_to_pfn(pa);

	return __smmu_iommu_map_pfn_hv(as, iova, pfn, prot);
}

static int __smmu_iommu_map_largepage_hv(struct smmu_as *as, dma_addr_t iova,
					phys_addr_t pa, unsigned long prot)
{
	int attrs = _PDE_ATTR;

	if (dma_get_attr(DMA_ATTR_READ_ONLY, (struct dma_attrs *)prot))
		attrs &= ~_WRITABLE;
	else if (dma_get_attr(DMA_ATTR_WRITE_ONLY, (struct dma_attrs *)prot))
		attrs &= ~_READABLE;

	return libsmmu_map_large_page(as->tegra_hv_comm_chan, as->asid, iova,
								pa, attrs);
}

static int smmu_iommu_map_hv(struct iommu_domain *domain, unsigned long iova,
			phys_addr_t pa, size_t bytes, unsigned long prot)
{
	struct smmu_as *as = domain_to_as(domain, iova);
	unsigned long flags;
	int err;
	int (*fn)(struct smmu_as *as, dma_addr_t iova, phys_addr_t pa,
					unsigned long prot);

	dev_dbg(as->smmu->dev, "[%d] %pad:%pap\n", as->asid, &iova, &pa);

	switch (bytes) {
	case SZ_4K:
		fn = __smmu_iommu_map_page_hv;
		break;
	case SZ_4M:
		fn = __smmu_iommu_map_largepage_hv;
		break;
	default:
		WARN(1,  "%lld not supported\n", (u64)bytes);
		return -EINVAL;
	}

	spin_lock_irqsave(&as->lock, flags);
	err = fn(as, iova, pa, prot);
	spin_unlock_irqrestore(&as->lock, flags);
	return err;
}

static int smmu_iommu_map_sg_hv(struct iommu_domain *domain,
					unsigned long iova,
					struct scatterlist *sgl, int npages,
					unsigned long prot)
{
	struct smmu_as *as = domain_to_as(domain, iova);
	size_t total = npages;
	int attrs = as->pte_attr;
	int i, err = 0;
	unsigned long flags;
	size_t sg_remaining = sg_num_pages(sgl);
	unsigned long sg_pfn = page_to_pfn(sg_page(sgl));

	if (dma_get_attr(DMA_ATTR_READ_ONLY, (struct dma_attrs *)prot))
		attrs &= ~_WRITABLE;
	else if (dma_get_attr(DMA_ATTR_WRITE_ONLY, (struct dma_attrs *)prot))
		attrs &= ~_READABLE;

	while (total > 0) {
		spin_lock_irqsave(&as->lock, flags);
		for (i = 0; i < sg_remaining; i++) {
			u32 pa = (sg_pfn + i) << 12;
			/* optimize to program in one request */
			err = libsmmu_map_page(as->tegra_hv_comm_chan, as->asid,
							iova, pa, attrs);
			if (err < 0) {
				spin_unlock_irqrestore(&as->lock, flags);
				goto fail;
			}
			iova += PAGE_SIZE;
			total--;
		}
		sgl = sg_next(sgl);
		if (sgl) {
			sg_pfn = page_to_pfn(sg_page(sgl));
			sg_remaining = sg_num_pages(sgl);
		}
		spin_unlock_irqrestore(&as->lock, flags);
		BUG_ON(!sgl && total);
	}
fail:
	return err;
}

static int __smmu_iommu_unmap_hv(struct smmu_as *as, dma_addr_t iova,
							size_t bytes)
{
	return libsmmu_unmap(as->tegra_hv_comm_chan, as->asid, iova, bytes);
}

static size_t smmu_iommu_unmap_hv(struct iommu_domain *domain,
					 unsigned long iova, size_t bytes)
{
	struct smmu_as *as = domain_to_as(domain, iova);
	unsigned long flags;
	size_t unmapped;

	dev_dbg(as->smmu->dev, "[%d] %pad\n", as->asid, &iova);

	spin_lock_irqsave(&as->lock, flags);
	unmapped = __smmu_iommu_unmap_hv(as, iova, bytes);
	spin_unlock_irqrestore(&as->lock, flags);
	return unmapped;
}

static phys_addr_t smmu_iommu_iova_to_phys_hv(struct iommu_domain *domain,
							dma_addr_t iova)
{
	struct smmu_as *as = domain_to_as(domain, iova);
	u64 pa;
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&as->lock, flags);
	err = libsmmu_iova_to_phys(as->tegra_hv_comm_chan, as->asid, iova, &pa);
	spin_unlock_irqrestore(&as->lock, flags);

	if (err < 0) {
		dev_err(as->smmu->dev,  "Failed iova_to_phys\n");
		return PTR_ERR(NULL);
	}
	dev_dbg(as->smmu->dev, "iova:%pad pfn:%pa asid:%d\n",
						&iova, &pa, as->asid);

	return pa;
}

static void smmu_domain_destroy_hv(struct smmu_device *smmu, struct smmu_as *as)
{
	/* Do nothing for now */
	return;
}

static int tegra_smmu_suspend_hv(struct device *dev)
{
	/* place holder for now */
	return 0;
}

static int tegra_smmu_resume_hv(struct device *dev)
{
	/* place holder for now */
	return 0;
}

static int tegra_smmu_reboot_notifier(struct notifier_block *nb,
				unsigned long event, void *unused)
{
	libsmmu_disconnect(smmu_handle->tegra_hv_comm_chan);
	return 0;
}

static struct notifier_block tegra_smmu_reboot_nb = {
	.notifier_call = tegra_smmu_reboot_notifier,
};

#define DEBUG_OP_HIT_MISS_COUNTER_ON	1
#define DEBUG_OP_HIT_MISS_COUNTER_OFF	2
#define DEBUG_OP_HIT_MISS_COUNTER_RESET	3
#define DEBUG_OP_GET_HIT_MISS_COUNTER	4

#define TLB_CACHE	0
#define PTC_CACHE	1

static ssize_t smmu_debugfs_stats_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *pos)
{
	struct smmu_debugfs_info *info;
	struct smmu_device *smmu;
	int i;
	int op;
	u64 op_data_out;

	enum {
		_OFF = 0,
		_ON,
		_RESET,
	};

	const char * const command[] = {
		[_OFF]          = "off",
		[_ON]           = "on",
		[_RESET]        = "reset",
	};
	char str[] = "reset";

	count = min_t(size_t, count, sizeof(str));
	if (copy_from_user(str, buffer, count))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(command); i++)
		if (strncmp(str, command[i],
					strlen(command[i])) == 0)
			break;

	if (i == ARRAY_SIZE(command))
		return -EINVAL;
	info = file_inode(file)->i_private;
	smmu = info->smmu;

	switch (i) {
	case _OFF:
		op = DEBUG_OP_HIT_MISS_COUNTER_OFF;
		break;
	case _ON:
		op = DEBUG_OP_HIT_MISS_COUNTER_ON;
		break;
	case _RESET:
		op = DEBUG_OP_HIT_MISS_COUNTER_RESET;
		break;
	default:
		BUG();
		break;
	}

	libsmmu_debug_op(smmu->tegra_hv_debug_chan, op, info->cache,
								&op_data_out);
	return count;
}


static int smmu_debugfs_stats_show(struct seq_file *s, void *v)
{
	struct smmu_debugfs_info *info = s->private;
	struct smmu_device *smmu = info->smmu;
	int i;
	u64 op_data_out;
	const char * const stats[] = { "hit", "miss", };

	for (i = 0; i < ARRAY_SIZE(stats); i++) {

		libsmmu_debug_op(smmu->tegra_hv_debug_chan,
					DEBUG_OP_GET_HIT_MISS_COUNTER,
					info->cache, &op_data_out);
		seq_printf(s, "%s:%08llx ", stats[i], op_data_out);
	}
	seq_printf(s, "\n");
	return 0;
}

static int smmu_debugfs_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, smmu_debugfs_stats_show, inode->i_private);
}

static const struct file_operations smmu_debugfs_stats_fops_hv = {
	.open           = smmu_debugfs_stats_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
	.write          = smmu_debugfs_stats_write,
};

int tegra_smmu_probe_hv(struct platform_device *pdev,
				struct smmu_device *smmu)
{
	int err = -EINVAL;
	struct device *dev = &pdev->dev;
	int chan = 0;
	int num_as = 0;
	int i;

	err = tegra_hv_smmu_comm_init(dev);
	if (err)
		goto exit_probe_hv;

	/* allocate a ivc channel */
	chan = tegra_hv_smmu_comm_chan_alloc();
	if (chan < 0)
		goto exit_probe_hv;

	smmu->tegra_hv_comm_chan = chan;

	smmu->tegra_hv_debug_chan = tegra_hv_smmu_comm_chan_alloc();
	if (smmu->tegra_hv_debug_chan < 0)
		goto exit_probe_hv;

	/* Get the number of asid from server */
	err = libsmmu_get_num_asids(chan, &num_as);
	if (err)
		goto exit_probe_hv;

	if (num_as > smmu->num_as)
		goto exit_probe_hv;
	else
		smmu->num_as = num_as;

	for (i = 0; i < smmu->num_as; i++) {
		struct smmu_as *as = &smmu->as[i];

		as->tegra_hv_comm_chan = COMM_CHAN_UNASSIGNED;
		spin_lock_init(&as->tegra_hv_comm_chan_lock);
	}

	/* Get the swgids for this guest */
	err = libsmmu_get_swgids(chan, &smmu->swgids);
	if (err)
		goto exit_probe_hv;

	err = register_reboot_notifier(&tegra_smmu_reboot_nb);
	if (err) {
		pr_err("unable to register reboot notifier\n");
		return err;
	}

	/* Update default functions */
	__smmu_iommu_map_pfn = __smmu_iommu_map_pfn_hv;
	__smmu_client_set_hwgrp = smmu_client_set_hwgrp_hv;
	smmu_as_alloc = smmu_as_alloc_hv;
	smmu_as_free = smmu_as_free_hv;
	smmu_domain_destroy = smmu_domain_destroy_hv;
	__tegra_smmu_suspend = tegra_smmu_suspend_hv;
	__tegra_smmu_resume = tegra_smmu_resume_hv;
	smmu_debugfs_stats_fops = &smmu_debugfs_stats_fops_hv;

	/* update iommu_ops for hv */
	smmu_iommu_ops->map = smmu_iommu_map_hv;
	smmu_iommu_ops->map_sg = smmu_iommu_map_sg_hv;
	smmu_iommu_ops->unmap = smmu_iommu_unmap_hv;
	smmu_iommu_ops->iova_to_phys = smmu_iommu_iova_to_phys_hv;

	BUG_ON(cmpxchg(&smmu_handle, NULL, smmu));
	return 0;

exit_probe_hv:
	return err;
}
