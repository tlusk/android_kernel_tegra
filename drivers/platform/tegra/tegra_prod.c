/*
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/tegra_prod.h>

#define TEGRA_PROD_SETTING "prod-settings"

/**
 * tegra_prod_parse_dt - Read the prod setting form Device tree.
 * @np:		device node from which the property value is to be read.
 * @tegra_prod_list:	the list of tegra prods.
 *
 * Read the prod setting form DT according the prod name in tegra prod list.
 * prod tuple will be allocated dynamically according to the tuple number of
 * each prod in DT.
 *
 * Returns 0 on success.
 */
static int tegra_prod_parse_dt(const struct device_node *np,
		const struct device_node *prod_child,
		struct tegra_prod_list *tegra_prod_list)
{
	struct device_node *child;
	struct tegra_prod *t_prod;
	struct prod_tuple *p_tuple;
	int index;
	int n_child, j;
	int n_tupple = 3;
	int cnt;
	int ret;
	int count;
	u32 pval;

	if (!tegra_prod_list || !tegra_prod_list->tegra_prod) {
		pr_err("Node %s: Invalid tegra prods list.\n", np->name);
		return -EINVAL;
	};

	ret = of_property_read_u32(prod_child, "#prod-cells", &pval);
	if (!ret)
		n_tupple = pval;
	if ((n_tupple != 3) && (n_tupple != 4)) {
		pr_err("Node %s: Prod cells not supported\n", np->name);
		return -EINVAL;
	}
	tegra_prod_list->n_prod_cells = n_tupple;

	n_child = 0;
	for_each_child_of_node(prod_child, child) {
		/* Check whether child is enabled or not */
		if (!of_device_is_available(child))
			continue;

		t_prod = &tegra_prod_list->tegra_prod[n_child];
		t_prod->name = child->name;
		count = of_property_count_u32(child, "prod");
		if ((count < n_tupple) || (count % n_tupple != 0)) {
			pr_err("Node %s: Not found proper setting in %s\n",
				child->name, t_prod->name);
			ret = -EINVAL;
			goto err_parsing;
		}

		t_prod->count = count / n_tupple;
		t_prod->prod_tuple = kzalloc(sizeof(*p_tuple) * t_prod->count,
						GFP_KERNEL);
		if (!t_prod->prod_tuple) {
			ret = -ENOMEM;
			goto err_parsing;
		}

		t_prod->boot_init = of_property_read_bool(child,
						"nvidia,prod-boot-init");
		for (cnt = 0; cnt < t_prod->count; cnt++) {
			p_tuple = (struct prod_tuple *)&t_prod->prod_tuple[cnt];
			index = cnt * n_tupple;

			if (n_tupple == 4) {
				ret = of_property_read_u32_index(child, "prod",
						index, &p_tuple->index);
				if (ret) {
					pr_err("Node %s: Failed to parse %s index\n",
						child->name, t_prod->name);
					goto err_parsing;
				}
				index++;
			} else {
				p_tuple->index = 0;
			}

			ret = of_property_read_u32_index(child, "prod",
						index, &p_tuple->addr);
			if (ret) {
				pr_err("Node %s: Failed to parse %s address\n",
					child->name, t_prod->name);
				goto err_parsing;
			}
			index++;

			ret = of_property_read_u32_index(child, "prod",
						index, &p_tuple->mask);
			if (ret) {
				pr_err("Node %s: Failed to parse %s mask\n",
					child->name, t_prod->name);
				goto err_parsing;
			}
			index++;

			ret = of_property_read_u32_index(child, "prod",
						index, &p_tuple->val);
			if (ret) {
				pr_err("Node %s: Failed to parse %s value\n",
					child->name, t_prod->name);
				goto err_parsing;
			}
		}
		n_child++;
	}

	tegra_prod_list->num = n_child;

	of_node_put(child);
	return 0;

err_parsing:
	of_node_put(child);
	for (j = 0; j <= n_child; j++) {
		t_prod = (struct tegra_prod *)&tegra_prod_list->tegra_prod[j];
		kfree(t_prod->prod_tuple);
	}
	return ret;
}

/**
 * tegra_prod_set_tuple - Only set a tuple.
 * @base:		base address of the register.
 * @prod_tuple:		the tuple to set.
 *
 * Returns 0 on success.
 */
int tegra_prod_set_tuple(void __iomem **base, struct prod_tuple *prod_tuple)
{
	u32 reg;

	if (!prod_tuple)
		return -EINVAL;

	reg = readl(base[prod_tuple->index] + prod_tuple->addr);
	reg = ((reg & prod_tuple->mask) |
		(prod_tuple->val & ~prod_tuple->mask));
	writel(reg, base[prod_tuple->index] + prod_tuple->addr);

	return 0;
};

/**
 * tegra_prod_set - Set one prod setting.
 * @base:		base address of the register.
 * @tegra_prod:		the prod setting to set.
 *
 * Set all the tuples in one tegra_prod.
 * Returns 0 on success.
 */
int tegra_prod_set(void __iomem **base, struct tegra_prod *tegra_prod)
{
	int i;
	int ret;

	if (!tegra_prod)
		return -EINVAL;

	for (i = 0; i < tegra_prod->count; i++) {
		ret = tegra_prod_set_tuple(base, &tegra_prod->prod_tuple[i]);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * tegra_prod_set_list - Set all the prod settings of the list in sequence.
 * @base:		base address of the register.
 * @tegra_prod_list:	the list of tegra prods.
 *
 * Returns 0 on success.
 */
int tegra_prod_set_list(void __iomem **base,
		struct tegra_prod_list *tegra_prod_list)
{
	int i;
	int ret;

	if (!tegra_prod_list)
		return -EINVAL;

	for (i = 0; i < tegra_prod_list->num; i++) {
		ret = tegra_prod_set(base, &tegra_prod_list->tegra_prod[i]);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * tegra_prod_set_boot_init - Set all the prod settings of the list in sequence
 *			Which are needed for boot initialisation.
 * @base:		base address of the register.
 * @tegra_prod_list:	the list of tegra prods.
 *
 * Returns 0 on success.
 */
int tegra_prod_set_boot_init(void __iomem **base,
		struct tegra_prod_list *tegra_prod_list)
{
	int i;
	int ret;

	if (!tegra_prod_list)
		return -EINVAL;

	for (i = 0; i < tegra_prod_list->num; i++) {
		if (!tegra_prod_list->tegra_prod[i].boot_init)
			continue;
		ret = tegra_prod_set(base, &tegra_prod_list->tegra_prod[i]);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * tegra_prod_set_by_name - Set the prod setting according the name.
 * @base:		base address of the register.
 * @name:		the name of tegra prod need to set.
 * @tegra_prod_list:	the list of tegra prods.
 *
 * Find the tegra prod in the list according to the name. Then set
 * that tegra prod.
 *
 * Returns 0 on success.
 */
int tegra_prod_set_by_name(void __iomem **base, const char *name,
		struct tegra_prod_list *tegra_prod_list)
{
	int i;
	struct tegra_prod *t_prod;

	if (!tegra_prod_list)
		return -EINVAL;

	for (i = 0; i < tegra_prod_list->num; i++) {
		t_prod = &tegra_prod_list->tegra_prod[i];
		if (!t_prod)
			return -EINVAL;
		if (!strcmp(t_prod->name, name))
			return tegra_prod_set(base, t_prod);
	}

	return -ENODEV;
}

/**
 * tegra_prod_init - Init tegra prod list.
 * @np		device node from which the property value is to be read.
 *
 * Query all the prod settings under DT node & Init the tegra prod list
 * automatically.
 *
 * Returns 0 on success, -EINVAL for wrong prod number, -ENOMEM if faild
 * to allocate memory for tegra prod list.
 */
struct tegra_prod_list *tegra_prod_init(const struct device_node *np)
{
	struct tegra_prod_list *tegra_prod_list;
	struct device_node *child;
	int prod_num = 0;
	int ret;

	child = of_get_child_by_name(np, TEGRA_PROD_SETTING);
	if (!child)
		return ERR_PTR(-ENODEV);

	/* Check whether child is enabled or not */
	if (!of_device_is_available(child)) {
		pr_err("Node %s: Node is not enabled\n", np->name);
		return ERR_PTR(-ENODEV);
	}

	prod_num = of_get_child_count(child);
	if (prod_num <= 0) {
		pr_err("Node %s: No child node for prod settings\n", np->name);
		return  ERR_PTR(-ENODEV);
	}

	tegra_prod_list = kzalloc(sizeof(*tegra_prod_list), GFP_KERNEL);
	if (!tegra_prod_list)
		return  ERR_PTR(-ENOMEM);

	tegra_prod_list->tegra_prod = kzalloc(prod_num * sizeof(struct tegra_prod),
						GFP_KERNEL);
	if (!tegra_prod_list->tegra_prod) {
		ret = -ENOMEM;
		goto err_prod_alloc;
	}
	tegra_prod_list->num = prod_num;

	ret = tegra_prod_parse_dt(np, child, tegra_prod_list);
	if (ret) {
		pr_err("Node %s: Faild to read the Prod Setting.\n", np->name);
		goto err_get;
	}

	of_node_put(child);
	return tegra_prod_list;

err_get:
	kfree(tegra_prod_list->tegra_prod);
err_prod_alloc:
	kfree(tegra_prod_list);
	return ERR_PTR(ret);
}

/**
 * tegra_prod_get - Get the prod list for tegra.
 * @dev: Device for which prod settings are required.
 * @name: Name of the prod settings, if NULL then complete list of that device.
 */
struct tegra_prod_list *tegra_prod_get(struct device *dev, const char *name)
{
	if (!dev || !dev->of_node) {
		pr_err("Device does not have node pointer\n");
		return ERR_PTR(-EINVAL);
	}

	return tegra_prod_init(dev->of_node);
}

/**
 * tegra_prod_release - Release all the resources.
 * @tegra_prod_list:	the list of tegra prods.
 *
 * Release all the resources of tegra_prod_list.
 * Returns 0 on success.
 */
int tegra_prod_release(struct tegra_prod_list **tegra_prod_list)
{
	int i;
	struct tegra_prod *t_prod;
	struct tegra_prod_list *tp_list;

	if (!tegra_prod_list)
		return -EINVAL;

	tp_list = *tegra_prod_list;
	if (tp_list) {
		if (tp_list->tegra_prod) {
			for (i = 0; i < tp_list->num; i++) {
				t_prod = (struct tegra_prod *)
					&tp_list->tegra_prod[i];
				if (t_prod)
					kfree(t_prod->prod_tuple);
			}
			kfree(tp_list->tegra_prod);
		}
		kfree(tp_list);
	}

	*tegra_prod_list = NULL;
	return 0;
}
