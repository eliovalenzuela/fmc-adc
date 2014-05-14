/*
 * Copyright CERN 2012
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * handle DMA mapping
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>

#include <linux/fmc.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

#include "fmc-adc-100m14b4cha.h"
#include "fa-spec.h"

/*
 * zfat_calculate_nents
 *
 * It calculates the number of necessary nents
 */
static int zfat_calculate_nents(struct zfad_block *zfad_block,
				unsigned int n_blocks)
{
	int i, bytesleft;
	void *bufp;
	int mapbytes;
	int nents = 0;

	for (i = 0; i < n_blocks; ++i) {
		bytesleft = zfad_block[i].block->datalen;
		bufp = zfad_block[i].block->data;
		zfad_block[i].first_nent = nents;
		while (bytesleft) {
			nents++;
			if (bytesleft < (PAGE_SIZE - offset_in_page(bufp)))
				mapbytes = bytesleft;
			else
				mapbytes = PAGE_SIZE - offset_in_page(bufp);
			bufp += mapbytes;
			bytesleft -= mapbytes;
		}
	}
	return nents;
}

/*
 * zfad_setup_dma_scatter
 *
 * Initialize each element of the scatter list
 */
static void zfad_setup_dma_scatter(struct fa_dev *fa,
				   struct zfad_block *zfad_block,
				   unsigned int n_blocks)
{
	struct fa_spec_data *spec_data = fa->carrier_data;
	struct scatterlist *sg;
	int bytesleft = 0;
	void *bufp = NULL;
	int mapbytes;
	int i, i_blk;

	dev_dbg(&fa->zdev->head.dev, "Setup dma scatterlist for %zu bytes\n",
			zfad_block->block->datalen);

	i_blk = 0;
	for_each_sg(spec_data->sgt.sgl, sg, spec_data->sgt.nents, i) {
		if (i_blk < n_blocks && i == zfad_block[i_blk].first_nent) {
			WARN(bytesleft, "unmapped byte in block %i\n",
			     i_blk - 1);
			/*
			 * Configure the DMA for a new block, reset index and
			 * data pointer
			 */
			bytesleft = zfad_block[i_blk].block->datalen;
			bufp = zfad_block[i_blk].block->data;

			i_blk++; /* index the next block */
			if (unlikely(i_blk > n_blocks)) {
				dev_err(&fa->zdev->head.dev,
					"DMA map out of block\n");
				BUG();
			}
		}

		/*
		 * If there are less bytes left than what fits
		 * in the current page (plus page alignment offset)
		 * we just feed in this, else we stuff in as much
		 * as we can.
		 */
		if (bytesleft < (PAGE_SIZE - offset_in_page(bufp)))
			mapbytes = bytesleft;
		else
			mapbytes = PAGE_SIZE - offset_in_page(bufp);
		/* Map the page */
		if (is_vmalloc_addr(bufp))
			sg_set_page(sg, vmalloc_to_page(bufp), mapbytes,
				    offset_in_page(bufp));
		else
			sg_set_buf(sg, bufp, mapbytes);
		/* Configure next values */
		bufp += mapbytes;
		bytesleft -= mapbytes;
		pr_debug("sg item (%p(+0x%lx), len:%d, left:%d)\n",
			 virt_to_page(bufp), offset_in_page(bufp),
			 mapbytes, bytesleft);
	}
}

/*
 * zfad_map_dma
 * @cset: channel set
 * @zfad_block: the block to map through DMA
 *
 * Map a scatter/gather table for the DMA transfer from the FMC-ADC.
 * The DMA controller can store a single item, but more then one transfer
 * could be necessary
 */
static int zfad_map_dma(struct zio_cset *cset, struct zfad_block *zfad_block,
		 unsigned int n_blocks)
{
	struct fa_dev *fa = cset->zdev->priv_d;
	struct fa_spec_data *spec_data = fa->carrier_data;
	struct device *dev = &fa->fmc->dev;
	struct scatterlist *sg;
	struct fa_dma_item *items;
	uint32_t dev_mem_off = 0;
	unsigned int i, pages, sglen, size, i_blk;
	dma_addr_t tmp;
	int err;

	pages = zfat_calculate_nents(zfad_block, n_blocks);
	if (!pages) {
		dev_info(dev, "No pages to transfer %i\n",
			n_blocks);
		return -EINVAL;
	}
	dev_dbg(dev, "using %d pages to transfer %i blocks\n",
		pages, n_blocks);

	/* Create sglists for the transfers */
	err = sg_alloc_table(&spec_data->sgt, pages, GFP_ATOMIC);
	if (err) {
		dev_err(dev, "cannot allocate sg table (%i pages)\n", pages);
		goto out;
	}

	/* Limited to 32-bit (kernel limit) */
	size = sizeof(*items) * spec_data->sgt.nents;
	items = kzalloc(size, GFP_ATOMIC);
	if (!items) {
		dev_err(fa->fmc->hwdev, "cannot allocate coherent dma memory\n");
		goto out_mem;
	}
	spec_data->items = items;
	spec_data->dma_list_item = dma_map_single(fa->fmc->hwdev, items, size,
						   DMA_TO_DEVICE);
	if (!spec_data->dma_list_item)
		goto out_free;

	/* Setup the scatter list for the provided block */
	zfad_setup_dma_scatter(fa, zfad_block, n_blocks);
	/* Map DMA buffers */
	sglen = dma_map_sg(fa->fmc->hwdev, spec_data->sgt.sgl,
			   spec_data->sgt.nents, DMA_FROM_DEVICE);
	if (!sglen) {
		dev_err(dev, "cannot map dma memory\n");
		goto out_map;
	}

	/* Configure DMA items */
	i_blk = 0;
	for_each_sg(spec_data->sgt.sgl, sg, spec_data->sgt.nents, i) {
		if (i_blk < n_blocks && i == zfad_block[i_blk].first_nent) {
			/*
			 * FIXME if we trust our configuration, dev_mem_off is
			 * useless in multishot
			 */
			dev_mem_off = zfad_block[i_blk].dev_mem_off;

			i_blk++; /* index the next block */
			if (unlikely(i_blk > n_blocks)) {
				dev_err(dev, "DMA map out of block\n");
				BUG();
			}
		}

		/* Prepare DMA item */
		items[i].start_addr = dev_mem_off;
		items[i].dma_addr_l = sg_dma_address(sg) & 0xFFFFFFFF;
		items[i].dma_addr_h = (uint64_t)sg_dma_address(sg) >> 32;
		items[i].dma_len = sg_dma_len(sg);
		dev_mem_off += items[i].dma_len;
		if (!sg_is_last(sg)) {/* more transfers */
			/* uint64_t so it works on 32 and 64 bit */
			tmp = spec_data->dma_list_item;
			tmp += (sizeof(struct fa_dma_item) * (i + 1));
			items[i].next_addr_l = ((uint64_t)tmp) & 0xFFFFFFFF;
			items[i].next_addr_h = ((uint64_t)tmp) >> 32;
			items[i].attribute = 0x1;	/* more items */
		} else {
			items[i].attribute = 0x0;	/* last item */
		}

		pr_debug("configure DMA item %d "
			"(addr: 0x%llx len: %d)(dev off: 0x%x)"
			"(next item: 0x%x)\n",
			i, (long long)sg_dma_address(sg),
			sg_dma_len(sg), dev_mem_off, items[i].next_addr_l);

		/* The first item is written on the device */
		if (i == 0) {
			fa_writel(fa, spec_data->fa_dma_base,
				  &fa_spec_regs[ZFA_DMA_ADDR],
					    items[i].start_addr);
			fa_writel(fa, spec_data->fa_dma_base,
				  &fa_spec_regs[ZFA_DMA_ADDR_L],
					    items[i].dma_addr_l);
			fa_writel(fa, spec_data->fa_dma_base,
				  &fa_spec_regs[ZFA_DMA_ADDR_H],
					    items[i].dma_addr_h);
			fa_writel(fa, spec_data->fa_dma_base,
				  &fa_spec_regs[ZFA_DMA_LEN],
					    items[i].dma_len);
			fa_writel(fa, spec_data->fa_dma_base,
				  &fa_spec_regs[ZFA_DMA_NEXT_L],
					    items[i].next_addr_l);
			fa_writel(fa, spec_data->fa_dma_base,
				  &fa_spec_regs[ZFA_DMA_NEXT_H],
					    items[i].next_addr_h);
			/* Set that there is a next item */
			fa_writel(fa, spec_data->fa_dma_base,
				  &fa_spec_regs[ZFA_DMA_BR_LAST],
					    items[i].attribute);
		}
	}

	return 0;

out_map:
	dma_unmap_single(fa->fmc->hwdev, spec_data->dma_list_item, size,
		       DMA_TO_DEVICE);
out_free:
	kfree(spec_data->items);
out_mem:
	sg_free_table(&spec_data->sgt);
out:
	return -ENOMEM;
}

/*
 * zfad_unmap_dma
 * @cset: channel set
 * @zfad_block: the block to map through DMA
 *
 * It unmaps a blocks
 */
static void zfad_unmap_dma(struct zio_cset *cset)
{
	struct fa_dev *fa = cset->zdev->priv_d;
	unsigned int size;
	struct fa_spec_data *spec_data = fa->carrier_data;

	dev_dbg(fa->fmc->hwdev, "unmap DMA\n");
	size = sizeof(struct fa_dma_item) * spec_data->sgt.nents;
	dma_unmap_single(fa->fmc->hwdev, spec_data->dma_list_item, size,
			 DMA_TO_DEVICE);
	dma_unmap_sg(fa->fmc->hwdev, spec_data->sgt.sgl, spec_data->sgt.nents,
		     DMA_FROM_DEVICE);

	kfree(spec_data->items);
	spec_data->items = NULL;
	spec_data->dma_list_item = 0;
	sg_free_table(&spec_data->sgt);
}

int fa_spec_dma_start(struct zio_cset *cset)
{
	struct fa_dev *fa = cset->zdev->priv_d;
	struct fa_spec_data *spec_data = fa->carrier_data;
	struct zio_channel *interleave = cset->interleave;
	struct zfad_block *zfad_block = interleave->priv_d;
	int res;

	res = zfad_map_dma(cset, zfad_block, fa->n_shots);
	if (res)
		return res;

	/* Start DMA transfer */
	fa_writel(fa, spec_data->fa_dma_base,
			&fa_spec_regs[ZFA_DMA_CTL_START], 1);
	return 0;
}

void fa_spec_dma_done(struct zio_cset *cset)
{
	zfad_unmap_dma(cset);
}

void fa_spec_dma_error(struct zio_cset *cset)
{
	struct fa_dev *fa = cset->zdev->priv_d;
	struct fa_spec_data *spec_data = fa->carrier_data;
	uint32_t val;

	zfad_unmap_dma(cset);
	val = fa_readl(fa, spec_data->fa_dma_base,
			&fa_spec_regs[ZFA_DMA_STA]);
	if (val)
		dev_err(&fa->fmc->dev,
			"DMA error (status 0x%x). All acquisition lost\n", val);
}
