/*
 * IOMMU API for QCOM secure IOMMUs.  Somewhat based on arm-smmu.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2013 ARM Limited
 * Copyright (C) 2017 Red Hat
 */

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-iommu.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/io-pgtable.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/kconfig.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_iommu.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/qcom_scm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "arm-smmu-regs.h"

#define SMMU_INTR_SEL_NS     0x2000

struct qcom_iommu_ctx;

struct qcom_iommu_dev {
	/* IOMMU core code handle */
	struct iommu_device	 iommu;
	struct device		*dev;
	struct clk		*iface_clk;
	struct clk		*bus_clk;
	void __iomem		*local_base;
	u32			 sec_id;
	u8			 num_ctxs;
	struct qcom_iommu_ctx	*ctxs[0];   /* indexed by asid-1 */
};

struct qcom_iommu_ctx {
	struct device		*dev;
	void __iomem		*base;
	bool			 secure_init;
	u8			 asid;      /* asid and ctx bank # are 1:1 */
	struct iommu_domain	*domain;
};

struct qcom_iommu_domain {
	struct io_pgtable_ops	*pgtbl_ops;
	spinlock_t		 pgtbl_lock;
	struct mutex		 init_mutex; /* Protects iommu pointer */
	struct iommu_domain	 domain;
	struct qcom_iommu_dev	*iommu;
};

static struct qcom_iommu_domain *to_qcom_iommu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct qcom_iommu_domain, domain);
}

static const struct iommu_ops qcom_iommu_ops;

static struct qcom_iommu_dev * to_iommu(struct iommu_fwspec *fwspec)
{
	if (!fwspec || fwspec->ops != &qcom_iommu_ops)
		return NULL;
	return fwspec->iommu_priv;
}

static struct qcom_iommu_ctx * to_ctx(struct iommu_fwspec *fwspec, unsigned asid)
{
	struct qcom_iommu_dev *qcom_iommu = to_iommu(fwspec);
	if (!qcom_iommu)
		return NULL;
	return qcom_iommu->ctxs[asid - 1];
}

static inline void
iommu_writel(struct qcom_iommu_ctx *ctx, unsigned reg, u32 val)
{
	writel_relaxed(val, ctx->base + reg);
}

static inline void
iommu_writeq(struct qcom_iommu_ctx *ctx, unsigned reg, u64 val)
{
	writeq_relaxed(val, ctx->base + reg);
}

static inline u32
iommu_readl(struct qcom_iommu_ctx *ctx, unsigned reg)
{
	return readl_relaxed(ctx->base + reg);
}

static inline u64
iommu_readq(struct qcom_iommu_ctx *ctx, unsigned reg)
{
	return readq_relaxed(ctx->base + reg);
}

static void qcom_iommu_tlb_sync(void *cookie)
{
	struct iommu_fwspec *fwspec = cookie;
	unsigned i;

	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(fwspec, fwspec->ids[i]);
		unsigned int val, ret;

		iommu_writel(ctx, ARM_SMMU_CB_TLBSYNC, 0);

		ret = readl_poll_timeout(ctx->base + ARM_SMMU_CB_TLBSTATUS, val,
					 (val & 0x1) == 0, 0, 5000000);
		if (ret)
			dev_err(ctx->dev, "timeout waiting for TLB SYNC\n");
	}
}

static void qcom_iommu_tlb_inv_context(void *cookie)
{
	struct iommu_fwspec *fwspec = cookie;
	unsigned i;

	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(fwspec, fwspec->ids[i]);
		iommu_writel(ctx, ARM_SMMU_CB_S1_TLBIASID, ctx->asid);
	}

	qcom_iommu_tlb_sync(cookie);
}

static void qcom_iommu_tlb_inv_range_nosync(unsigned long iova, size_t size,
					    size_t granule, bool leaf, void *cookie)
{
	struct iommu_fwspec *fwspec = cookie;
	unsigned i, reg;

	reg = leaf ? ARM_SMMU_CB_S1_TLBIVAL : ARM_SMMU_CB_S1_TLBIVA;

	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(fwspec, fwspec->ids[i]);
		size_t s = size;

		iova &= ~12UL;
		iova |= ctx->asid;
		do {
			iommu_writel(ctx, reg, iova);
			iova += granule;
		} while (s -= granule);
	}
}

static const struct iommu_gather_ops qcom_gather_ops = {
	.tlb_flush_all	= qcom_iommu_tlb_inv_context,
	.tlb_add_flush	= qcom_iommu_tlb_inv_range_nosync,
	.tlb_sync	= qcom_iommu_tlb_sync,
};

static irqreturn_t qcom_iommu_fault(int irq, void *dev)
{
	struct qcom_iommu_ctx *ctx = dev;
	u32 fsr, fsynr;
	u64 iova;

	fsr = iommu_readl(ctx, ARM_SMMU_CB_FSR);

	if (!(fsr & FSR_FAULT))
		return IRQ_NONE;

	fsynr = iommu_readl(ctx, ARM_SMMU_CB_FSYNR0);
	iova = iommu_readq(ctx, ARM_SMMU_CB_FAR);

	if (!report_iommu_fault(ctx->domain, ctx->dev, iova, 0)) {
		dev_err_ratelimited(ctx->dev,
				    "Unhandled context fault: fsr=0x%x, "
				    "iova=0x%016llx, fsynr=0x%x, cb=%d\n",
				    fsr, iova, fsynr, ctx->asid);
	}

	iommu_writel(ctx, ARM_SMMU_CB_FSR, fsr);
	iommu_writel(ctx, ARM_SMMU_CB_RESUME, RESUME_TERMINATE);

	return IRQ_HANDLED;
}

static int qcom_iommu_init_domain(struct iommu_domain *domain,
				  struct qcom_iommu_dev *qcom_iommu,
				  struct iommu_fwspec *fwspec)
{
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct io_pgtable_ops *pgtbl_ops;
	struct io_pgtable_cfg pgtbl_cfg;
	int i, ret = 0;
	u32 reg;

	mutex_lock(&qcom_domain->init_mutex);
	if (qcom_domain->iommu)
		goto out_unlock;

	pgtbl_cfg = (struct io_pgtable_cfg) {
		.pgsize_bitmap	= qcom_iommu_ops.pgsize_bitmap,
		.ias		= 32,
		.oas		= 40,
		.tlb		= &qcom_gather_ops,
		.iommu_dev	= qcom_iommu->dev,
	};

	qcom_domain->iommu = qcom_iommu;
	pgtbl_ops = alloc_io_pgtable_ops(ARM_32_LPAE_S1, &pgtbl_cfg, fwspec);
	if (!pgtbl_ops) {
		dev_err(qcom_iommu->dev, "failed to allocate pagetable ops\n");
		ret = -ENOMEM;
		goto out_clear_iommu;
	}

	/* Update the domain's page sizes to reflect the page table format */
	domain->pgsize_bitmap = pgtbl_cfg.pgsize_bitmap;
	domain->geometry.aperture_end = (1ULL << pgtbl_cfg.ias) - 1;
	domain->geometry.force_aperture = true;

	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(fwspec, fwspec->ids[i]);

		if (!ctx->secure_init) {
			ret = qcom_scm_restore_sec_cfg(qcom_iommu->sec_id, ctx->asid);
			if (ret) {
				dev_err(qcom_iommu->dev, "secure init failed: %d\n", ret);
				goto out_clear_iommu;
			}
			ctx->secure_init = true;
		}

		/* TTBRs */
		iommu_writeq(ctx, ARM_SMMU_CB_TTBR0,
				pgtbl_cfg.arm_lpae_s1_cfg.ttbr[0] |
				((u64)ctx->asid << TTBRn_ASID_SHIFT));
		iommu_writeq(ctx, ARM_SMMU_CB_TTBR1,
				pgtbl_cfg.arm_lpae_s1_cfg.ttbr[1] |
				((u64)ctx->asid << TTBRn_ASID_SHIFT));

		/* TTBCR */
		iommu_writel(ctx, ARM_SMMU_CB_TTBCR2,
				(pgtbl_cfg.arm_lpae_s1_cfg.tcr >> 32) |
				TTBCR2_SEP_UPSTREAM);
		iommu_writel(ctx, ARM_SMMU_CB_TTBCR,
				pgtbl_cfg.arm_lpae_s1_cfg.tcr);

		/* MAIRs (stage-1 only) */
		iommu_writel(ctx, ARM_SMMU_CB_S1_MAIR0,
				pgtbl_cfg.arm_lpae_s1_cfg.mair[0]);
		iommu_writel(ctx, ARM_SMMU_CB_S1_MAIR1,
				pgtbl_cfg.arm_lpae_s1_cfg.mair[1]);

		/* SCTLR */
		reg = SCTLR_CFIE | SCTLR_CFRE | SCTLR_AFE | SCTLR_TRE |
			SCTLR_M | SCTLR_S1_ASIDPNE | SCTLR_CFCFG;

		if (IS_ENABLED(CONFIG_BIG_ENDIAN))
			reg |= SCTLR_E;

		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, reg);

		ctx->domain = domain;
	}

	mutex_unlock(&qcom_domain->init_mutex);

	/* Publish page table ops for map/unmap */
	qcom_domain->pgtbl_ops = pgtbl_ops;

	return 0;

out_clear_iommu:
	qcom_domain->iommu = NULL;
out_unlock:
	mutex_unlock(&qcom_domain->init_mutex);
	return ret;
}

static struct iommu_domain *qcom_iommu_domain_alloc(unsigned type)
{
	struct qcom_iommu_domain *qcom_domain;

	if (type != IOMMU_DOMAIN_UNMANAGED && type != IOMMU_DOMAIN_DMA)
		return NULL;
	/*
	 * Allocate the domain and initialise some of its data structures.
	 * We can't really do anything meaningful until we've added a
	 * master.
	 */
	qcom_domain = kzalloc(sizeof(*qcom_domain), GFP_KERNEL);
	if (!qcom_domain)
		return NULL;

	if (type == IOMMU_DOMAIN_DMA &&
	    iommu_get_dma_cookie(&qcom_domain->domain)) {
		kfree(qcom_domain);
		return NULL;
	}

	mutex_init(&qcom_domain->init_mutex);
	spin_lock_init(&qcom_domain->pgtbl_lock);

	return &qcom_domain->domain;
}

static void qcom_iommu_domain_free(struct iommu_domain *domain)
{
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);

	iommu_put_dma_cookie(domain);

	if (qcom_domain->iommu) {
		/*
		 * NOTE: unmap can be called after client device is powered
		 * off, for example, with GPUs or anything involving dma-buf.
		 * So we cannot rely on the device_link.  Make sure the IOMMU
		 * is on to avoid unclocked accesses in the TLB inv path:
		 */
		pm_runtime_get_sync(qcom_domain->iommu->dev);
		free_io_pgtable_ops(qcom_domain->pgtbl_ops);
		pm_runtime_put_sync(qcom_domain->iommu->dev);
	}

	kfree(qcom_domain);
}

static int qcom_iommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = to_iommu(dev->iommu_fwspec);
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	int ret;

	if (!qcom_iommu) {
		dev_err(dev, "cannot attach to IOMMU, is it on the same bus?\n");
		return -ENXIO;
	}

	/* Ensure that the domain is finalized */
	pm_runtime_get_sync(qcom_iommu->dev);
	ret = qcom_iommu_init_domain(domain, qcom_iommu, dev->iommu_fwspec);
	pm_runtime_put_sync(qcom_iommu->dev);
	if (ret < 0)
		return ret;

	/*
	 * Sanity check the domain. We don't support domains across
	 * different IOMMUs.
	 */
	if (qcom_domain->iommu != qcom_iommu) {
		dev_err(dev, "cannot attach to IOMMU %s while already "
			"attached to domain on IOMMU %s\n",
			dev_name(qcom_domain->iommu->dev),
			dev_name(qcom_iommu->dev));
		return -EINVAL;
	}

	return 0;
}

static void qcom_iommu_detach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct iommu_fwspec *fwspec = dev->iommu_fwspec;
	struct qcom_iommu_dev *qcom_iommu = to_iommu(fwspec);
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	unsigned i;

	if (WARN_ON(!qcom_domain->iommu))
		return;

	pm_runtime_get_sync(qcom_iommu->dev);
	for (i = 0; i < fwspec->num_ids; i++) {
		struct qcom_iommu_ctx *ctx = to_ctx(fwspec, fwspec->ids[i]);

		/* Disable the context bank: */
		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, 0);

		ctx->domain = NULL;
	}
	pm_runtime_put_sync(qcom_iommu->dev);
}

static int qcom_iommu_map(struct iommu_domain *domain, unsigned long iova,
			  phys_addr_t paddr, size_t size, int prot)
{
	int ret;
	unsigned long flags;
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct io_pgtable_ops *ops = qcom_domain->pgtbl_ops;

	if (!ops)
		return -ENODEV;

	spin_lock_irqsave(&qcom_domain->pgtbl_lock, flags);
	ret = ops->map(ops, iova, paddr, size, prot);
	spin_unlock_irqrestore(&qcom_domain->pgtbl_lock, flags);
	return ret;
}

static size_t qcom_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
			       size_t size)
{
	size_t ret;
	unsigned long flags;
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct io_pgtable_ops *ops = qcom_domain->pgtbl_ops;

	if (!ops)
		return 0;

	/* NOTE: unmap can be called after client device is powered off,
	 * for example, with GPUs or anything involving dma-buf.  So we
	 * cannot rely on the device_link.  Make sure the IOMMU is on to
	 * avoid unclocked accesses in the TLB inv path:
	 */
	pm_runtime_get_sync(qcom_domain->iommu->dev);
	spin_lock_irqsave(&qcom_domain->pgtbl_lock, flags);
	ret = ops->unmap(ops, iova, size);
	spin_unlock_irqrestore(&qcom_domain->pgtbl_lock, flags);
	pm_runtime_put_sync(qcom_domain->iommu->dev);

	return ret;
}

static void qcom_iommu_iotlb_sync(struct iommu_domain *domain)
{
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct io_pgtable *pgtable = container_of(qcom_domain->pgtbl_ops,
						  struct io_pgtable, ops);
	if (!qcom_domain->pgtbl_ops)
		return;

	pm_runtime_get_sync(qcom_domain->iommu->dev);
	qcom_iommu_tlb_sync(pgtable->cookie);
	pm_runtime_put_sync(qcom_domain->iommu->dev);
}

static phys_addr_t qcom_iommu_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	phys_addr_t ret;
	unsigned long flags;
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);
	struct io_pgtable_ops *ops = qcom_domain->pgtbl_ops;

	if (!ops)
		return 0;

	spin_lock_irqsave(&qcom_domain->pgtbl_lock, flags);
	ret = ops->iova_to_phys(ops, iova);
	spin_unlock_irqrestore(&qcom_domain->pgtbl_lock, flags);

	return ret;
}

static bool qcom_iommu_capable(enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		/*
		 * Return true here as the SMMU can always send out coherent
		 * requests.
		 */
		return true;
	case IOMMU_CAP_NOEXEC:
		return true;
	default:
		return false;
	}
}

static int qcom_iommu_add_device(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = to_iommu(dev->iommu_fwspec);
	struct iommu_group *group;
	struct device_link *link;

	if (!qcom_iommu)
		return -ENODEV;

	/*
	 * Establish the link between iommu and master, so that the
	 * iommu gets runtime enabled/disabled as per the master's
	 * needs.
	 */
	link = device_link_add(dev, qcom_iommu->dev, DL_FLAG_PM_RUNTIME);
	if (!link) {
		dev_err(qcom_iommu->dev, "Unable to create device link between %s and %s\n",
			dev_name(qcom_iommu->dev), dev_name(dev));
		return -ENODEV;
	}

	group = iommu_group_get_for_dev(dev);
	if (IS_ERR_OR_NULL(group))
		return PTR_ERR_OR_ZERO(group);

	iommu_group_put(group);
	iommu_device_link(&qcom_iommu->iommu, dev);

	return 0;
}

static void qcom_iommu_remove_device(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = to_iommu(dev->iommu_fwspec);

	if (!qcom_iommu)
		return;

	iommu_device_unlink(&qcom_iommu->iommu, dev);
	iommu_group_remove_device(dev);
	iommu_fwspec_free(dev);
}

static int qcom_iommu_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	struct qcom_iommu_dev *qcom_iommu;
	struct platform_device *iommu_pdev;
	unsigned asid = args->args[0];

	if (args->args_count != 1) {
		dev_err(dev, "incorrect number of iommu params found for %s "
			"(found %d, expected 1)\n",
			args->np->full_name, args->args_count);
		return -EINVAL;
	}

	iommu_pdev = of_find_device_by_node(args->np);
	if (WARN_ON(!iommu_pdev))
		return -EINVAL;

	qcom_iommu = platform_get_drvdata(iommu_pdev);

	/* make sure the asid specified in dt is valid, so we don't have
	 * to sanity check this elsewhere, since 'asid - 1' is used to
	 * index into qcom_iommu->ctxs:
	 */
	if (WARN_ON(asid < 1) ||
	    WARN_ON(asid > qcom_iommu->num_ctxs))
		return -EINVAL;

	if (!dev->iommu_fwspec->iommu_priv) {
		dev->iommu_fwspec->iommu_priv = qcom_iommu;
	} else {
		/* make sure devices iommus dt node isn't referring to
		 * multiple different iommu devices.  Multiple context
		 * banks are ok, but multiple devices are not:
		 */
		if (WARN_ON(qcom_iommu != dev->iommu_fwspec->iommu_priv))
			return -EINVAL;
	}

	return iommu_fwspec_add_ids(dev, &asid, 1);
}

static const struct iommu_ops qcom_iommu_ops = {
	.capable	= qcom_iommu_capable,
	.domain_alloc	= qcom_iommu_domain_alloc,
	.domain_free	= qcom_iommu_domain_free,
	.attach_dev	= qcom_iommu_attach_dev,
	.detach_dev	= qcom_iommu_detach_dev,
	.map		= qcom_iommu_map,
	.unmap		= qcom_iommu_unmap,
	.flush_iotlb_all = qcom_iommu_iotlb_sync,
	.iotlb_sync	= qcom_iommu_iotlb_sync,
	.iova_to_phys	= qcom_iommu_iova_to_phys,
	.add_device	= qcom_iommu_add_device,
	.remove_device	= qcom_iommu_remove_device,
	.device_group	= generic_device_group,
	.of_xlate	= qcom_iommu_of_xlate,
	.pgsize_bitmap	= SZ_4K | SZ_64K | SZ_1M | SZ_16M,
};

static int qcom_iommu_enable_clocks(struct qcom_iommu_dev *qcom_iommu)
{
	int ret;

	ret = clk_prepare_enable(qcom_iommu->iface_clk);
	if (ret) {
		dev_err(qcom_iommu->dev, "Couldn't enable iface_clk\n");
		return ret;
	}

	ret = clk_prepare_enable(qcom_iommu->bus_clk);
	if (ret) {
		dev_err(qcom_iommu->dev, "Couldn't enable bus_clk\n");
		clk_disable_unprepare(qcom_iommu->iface_clk);
		return ret;
	}

	return 0;
}

static void qcom_iommu_disable_clocks(struct qcom_iommu_dev *qcom_iommu)
{
	clk_disable_unprepare(qcom_iommu->bus_clk);
	clk_disable_unprepare(qcom_iommu->iface_clk);
}

static int qcom_iommu_sec_ptbl_init(struct device *dev)
{
	size_t psize = 0;
	unsigned int spare = 0;
	void *cpu_addr;
	dma_addr_t paddr;
	unsigned long attrs;
	static bool allocated = false;
	int ret;

	if (allocated)
		return 0;

	ret = qcom_scm_iommu_secure_ptbl_size(spare, &psize);
	if (ret) {
		dev_err(dev, "failed to get iommu secure pgtable size (%d)\n",
			ret);
		return ret;
	}

	dev_info(dev, "iommu sec: pgtable size: %zu\n", psize);

	attrs = DMA_ATTR_NO_KERNEL_MAPPING;

	cpu_addr = dma_alloc_attrs(dev, psize, &paddr, GFP_KERNEL, attrs);
	if (!cpu_addr) {
		dev_err(dev, "failed to allocate %zu bytes for pgtable\n",
			psize);
		return -ENOMEM;
	}

	ret = qcom_scm_iommu_secure_ptbl_init(paddr, psize, spare);
	if (ret) {
		dev_err(dev, "failed to init iommu pgtable (%d)\n", ret);
		goto free_mem;
	}

	allocated = true;
	return 0;

free_mem:
	dma_free_attrs(dev, psize, cpu_addr, paddr, attrs);
	return ret;
}

static int get_asid(const struct device_node *np)
{
	u32 reg;

	/* read the "reg" property directly to get the relative address
	 * of the context bank, and calculate the asid from that:
	 */
	if (of_property_read_u32_index(np, "reg", 0, &reg))
		return -ENODEV;

	return reg / 0x1000;      /* context banks are 0x1000 apart */
}

static int qcom_iommu_ctx_probe(struct platform_device *pdev)
{
	struct qcom_iommu_ctx *ctx;
	struct device *dev = &pdev->dev;
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(dev->parent);
	struct resource *res;
	int ret, irq;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;
	platform_set_drvdata(pdev, ctx);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ctx->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(ctx->base))
		return PTR_ERR(ctx->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "failed to get irq\n");
		return -ENODEV;
	}

	/* clear IRQs before registering fault handler, just in case the
	 * boot-loader left us a surprise:
	 */
	iommu_writel(ctx, ARM_SMMU_CB_FSR, iommu_readl(ctx, ARM_SMMU_CB_FSR));

	ret = devm_request_irq(dev, irq,
			       qcom_iommu_fault,
			       IRQF_SHARED,
			       "qcom-iommu-fault",
			       ctx);
	if (ret) {
		dev_err(dev, "failed to request IRQ %u\n", irq);
		return ret;
	}

	ret = get_asid(dev->of_node);
	if (ret < 0) {
		dev_err(dev, "missing reg property\n");
		return ret;
	}

	ctx->asid = ret;

	dev_dbg(dev, "found asid %u\n", ctx->asid);

	qcom_iommu->ctxs[ctx->asid - 1] = ctx;

	return 0;
}

static int qcom_iommu_ctx_remove(struct platform_device *pdev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(pdev->dev.parent);
	struct qcom_iommu_ctx *ctx = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);

	qcom_iommu->ctxs[ctx->asid - 1] = NULL;

	return 0;
}

static const struct of_device_id ctx_of_match[] = {
	{ .compatible = "qcom,msm-iommu-v1-ns" },
	{ .compatible = "qcom,msm-iommu-v1-sec" },
	{ /* sentinel */ }
};

static struct platform_driver qcom_iommu_ctx_driver = {
	.driver	= {
		.name		= "qcom-iommu-ctx",
		.of_match_table	= of_match_ptr(ctx_of_match),
	},
	.probe	= qcom_iommu_ctx_probe,
	.remove = qcom_iommu_ctx_remove,
};

static bool qcom_iommu_has_secure_context(struct qcom_iommu_dev *qcom_iommu)
{
	struct device_node *child;

	for_each_child_of_node(qcom_iommu->dev->of_node, child)
		if (of_device_is_compatible(child, "qcom,msm-iommu-v1-sec"))
			return true;

	return false;
}

static int qcom_iommu_device_probe(struct platform_device *pdev)
{
	struct device_node *child;
	struct qcom_iommu_dev *qcom_iommu;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret, sz, max_asid = 0;

	/* find the max asid (which is 1:1 to ctx bank idx), so we know how
	 * many child ctx devices we have:
	 */
	for_each_child_of_node(dev->of_node, child)
		max_asid = max(max_asid, get_asid(child));

	sz = sizeof(*qcom_iommu) + (max_asid * sizeof(qcom_iommu->ctxs[0]));

	qcom_iommu = devm_kzalloc(dev, sz, GFP_KERNEL);
	if (!qcom_iommu)
		return -ENOMEM;
	qcom_iommu->num_ctxs = max_asid;
	qcom_iommu->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res)
		qcom_iommu->local_base = devm_ioremap_resource(dev, res);

	qcom_iommu->iface_clk = devm_clk_get(dev, "iface");
	if (IS_ERR(qcom_iommu->iface_clk)) {
		dev_err(dev, "failed to get iface clock\n");
		return PTR_ERR(qcom_iommu->iface_clk);
	}

	qcom_iommu->bus_clk = devm_clk_get(dev, "bus");
	if (IS_ERR(qcom_iommu->bus_clk)) {
		dev_err(dev, "failed to get bus clock\n");
		return PTR_ERR(qcom_iommu->bus_clk);
	}

	if (of_property_read_u32(dev->of_node, "qcom,iommu-secure-id",
				 &qcom_iommu->sec_id)) {
		dev_err(dev, "missing qcom,iommu-secure-id property\n");
		return -ENODEV;
	}

	if (qcom_iommu_has_secure_context(qcom_iommu)) {
		ret = qcom_iommu_sec_ptbl_init(dev);
		if (ret) {
			dev_err(dev, "cannot init secure pg table(%d)\n", ret);
			return ret;
		}
	}

	platform_set_drvdata(pdev, qcom_iommu);

	pm_runtime_enable(dev);

	/* register context bank devices, which are child nodes: */
	ret = devm_of_platform_populate(dev);
	if (ret) {
		dev_err(dev, "Failed to populate iommu contexts\n");
		return ret;
	}

	ret = iommu_device_sysfs_add(&qcom_iommu->iommu, dev, NULL,
				     dev_name(dev));
	if (ret) {
		dev_err(dev, "Failed to register iommu in sysfs\n");
		return ret;
	}

	iommu_device_set_ops(&qcom_iommu->iommu, &qcom_iommu_ops);
	iommu_device_set_fwnode(&qcom_iommu->iommu, dev->fwnode);

	ret = iommu_device_register(&qcom_iommu->iommu);
	if (ret) {
		dev_err(dev, "Failed to register iommu\n");
		return ret;
	}

	bus_set_iommu(&platform_bus_type, &qcom_iommu_ops);

	if (qcom_iommu->local_base) {
		pm_runtime_get_sync(dev);
		writel_relaxed(0xffffffff, qcom_iommu->local_base + SMMU_INTR_SEL_NS);
		pm_runtime_put_sync(dev);
	}

	return 0;
}

static int qcom_iommu_device_remove(struct platform_device *pdev)
{
	struct qcom_iommu_dev *qcom_iommu = platform_get_drvdata(pdev);

	bus_set_iommu(&platform_bus_type, NULL);

	pm_runtime_force_suspend(&pdev->dev);
	platform_set_drvdata(pdev, NULL);
	iommu_device_sysfs_remove(&qcom_iommu->iommu);
	iommu_device_unregister(&qcom_iommu->iommu);

	return 0;
}

static int __maybe_unused qcom_iommu_resume(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(dev);

	return qcom_iommu_enable_clocks(qcom_iommu);
}

static int __maybe_unused qcom_iommu_suspend(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(dev);

	qcom_iommu_disable_clocks(qcom_iommu);

	return 0;
}

static const struct dev_pm_ops qcom_iommu_pm_ops = {
	SET_RUNTIME_PM_OPS(qcom_iommu_suspend, qcom_iommu_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static const struct of_device_id qcom_iommu_of_match[] = {
	{ .compatible = "qcom,msm-iommu-v1" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, qcom_iommu_of_match);

static struct platform_driver qcom_iommu_driver = {
	.driver	= {
		.name		= "qcom-iommu",
		.of_match_table	= of_match_ptr(qcom_iommu_of_match),
		.pm		= &qcom_iommu_pm_ops,
		.probe_type	= PROBE_FORCE_SYNCHRONOUS,
	},
	.probe	= qcom_iommu_device_probe,
	.remove	= qcom_iommu_device_remove,
};

static int __init qcom_iommu_init(void)
{
	int ret;

	ret = platform_driver_register(&qcom_iommu_ctx_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&qcom_iommu_driver);
	if (ret)
		platform_driver_unregister(&qcom_iommu_ctx_driver);

	return ret;
}

static void __exit qcom_iommu_exit(void)
{
	platform_driver_unregister(&qcom_iommu_driver);
	platform_driver_unregister(&qcom_iommu_ctx_driver);
}

module_init(qcom_iommu_init);
module_exit(qcom_iommu_exit);

MODULE_DESCRIPTION("IOMMU API for QCOM IOMMU v1 implementations");
MODULE_LICENSE("GPL v2");
