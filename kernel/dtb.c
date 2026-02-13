#include <aosd/cpu.h>
#include <aosd/dtb.h>
#include <aosd/errno.h>
#include <aosd/libfdt.h>
#include <aosd/memblock.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/printk.h>
#include <aosd/slab.h>

uint64_t dtb_phys;
void *dtb_virt;

static int dtb_get_cells(int node, int *addr_cells, int *size_cells)
{
	int ret;

	ret = fdt_address_cells(dtb_virt, node);
	if (ret < 0)
		return ret;
	*addr_cells = ret;

	ret = fdt_size_cells(dtb_virt, node);
	if (ret < 0)
		return ret;
	*size_cells = ret;

	return 0;
}

static int dtb_get_parent_cells(int node, int *addr_cells, int *size_cells)
{
	int ret = fdt_parent_offset(dtb_virt, node);
	if (ret < 0)
		return ret;
	return dtb_get_cells(ret, addr_cells, size_cells);
}

static int dtb_for_each_reg(int node, int addr_cells, int size_cells,
			    int (*callback)(uint64_t addr, size_t size))
{
	const uint32_t *reg;
	uint64_t addr, size;
	int len = 0;
	int reg_bytes = (addr_cells + size_cells) * sizeof(uint32_t);
	int nr_regs;
	int err;

	reg = fdt_getprop(dtb_virt, node, "reg", &len);

	if (!reg)
		return -EINVAL;

	if (len < reg_bytes || len % reg_bytes != 0)
		return -EINVAL;

	nr_regs = len / reg_bytes;
	for (int i = 0; i < nr_regs; ++i) {
		addr = 0;
		size = 0;

		for (int j = 0; j < addr_cells; ++j)
			addr = (addr << 32) | fdt32_to_cpu(*reg++);

		for (int j = 0; j < size_cells; ++j)
			size = (size << 32) | fdt32_to_cpu(*reg++);

		err = callback(addr, size);
		if (err)
			return err;
	}

	return 0;
}

int dtb_early_init_scan_mem(void)
{
	int err;
	int node;
	int addr_cells, size_cells;

	node = fdt_path_offset(dtb_virt, "/memory");
	if (node < 0) {
		log_warn("memory node not found\n");
		return -EINVAL;
	}

	err = dtb_get_parent_cells(node, &addr_cells, &size_cells);
	if (err)
		return err;

	err = dtb_for_each_reg(node, addr_cells, size_cells, memblock_add);
	if (err)
		return err;

	return 0;
}

int dtb_early_init_scan_reserved_mem(void)
{
	int err;
	int node, subnode;
	int addr_cells, size_cells;

	node = fdt_path_offset(dtb_virt, "/reserved-memory");
	if (node < 0)
		return 0;

	err = dtb_get_cells(node, &addr_cells, &size_cells);
	if (err)
		return err;

	fdt_for_each_subnode(subnode, dtb_virt, node) {
		err = dtb_for_each_reg(subnode, addr_cells, size_cells,
				       memblock_reserve);
		if (err)
			return err;
	}

	return 0;
}

int dtb_init_scan_cpu(void)
{
	int node, subnode;
	char const *devtype;
	struct cpu *cpu = NULL;
	uint32_t hart_id = 0;
	uint32_t timebase_freq = 0;
	const uint32_t *freq;
	const uint32_t *reg;
	int len;

	node = fdt_path_offset(dtb_virt, "/cpus");
	if (node < 0) {
		log_warn("CPU node not found\n");
		return -EINVAL;
	}

	freq = fdt_getprop(dtb_virt, node, "timebase-frequency", &len);
	if (!freq || len != sizeof(uint32_t)) {
		log_warn("timebase-frequency property not found\n");
		return -EINVAL;
	}
	timebase_freq = fdt32_to_cpu(*freq);
	cpu_set_timebase_freq(timebase_freq);

	fdt_for_each_subnode(subnode, dtb_virt, node) {
		devtype = fdt_getprop(dtb_virt, subnode, "device_type", NULL);
		if (!devtype || strcmp(devtype, "cpu") != 0)
			continue;

		log_info("found CPU node %s\n",
			 fdt_get_name(dtb_virt, subnode, NULL));

		cpu = kzalloc(sizeof(*cpu));
		if (!cpu) {
			log_warn("failed to allocate memory for CPU\n");
			return -ENOMEM;
		}

		reg = fdt_getprop(dtb_virt, subnode, "reg", &len);
		if (!reg || len != sizeof(uint32_t)) {
			log_warn("CPU reg property not found\n");
			return -EINVAL;
		}
		hart_id = fdt32_to_cpu(*reg);
		cpu->hart_id = hart_id;
		cpu_add(cpu);
	}

	return 0;
}

static int dtb_get_one_reg(int node, uint64_t *addr, uint64_t *size)
{
	int len;
	uint32_t const *reg;
	int addr_cells, size_cells;

	if (dtb_get_parent_cells(node, &addr_cells, &size_cells))
		return -EINVAL;

	int cell_bytes = (addr_cells + size_cells) * sizeof(uint32_t);

	reg = fdt_getprop(dtb_virt, node, "reg", &len);
	if (!reg || len < cell_bytes || len % cell_bytes != 0)
		return -EINVAL;

	for (int i = 0; i < addr_cells; ++i)
		*addr = (*addr << 32) | fdt32_to_cpu(*reg++);
	for (int i = 0; i < size_cells; ++i)
		*size = (*size << 32) | fdt32_to_cpu(*reg++);

	return 0;
}

int dtb_parse_plic(struct plic_device *plic)
{
	int node;
	const uint32_t *ndev;
	int len;
	uint64_t addr = 0;
	uint64_t size = 0;

	node = fdt_path_offset(dtb_virt, "/soc/plic");
	if (node < 0) {
		log_warn("PLIC node not found\n");
		return -EINVAL;
	}

	if (dtb_get_one_reg(node, &addr, &size)) {
		log_warn("PLIC reg property not found\n");
		return -EINVAL;
	}

	plic->phys_base = addr;
	plic->size = size;

	ndev = fdt_getprop(dtb_virt, node, "riscv,ndev", &len);
	if (!ndev || len != sizeof(uint32_t)) {
		log_warn("PLIC riscv,ndev property not found\n");
		return -EINVAL;
	}
	plic->ndev = fdt32_to_cpu(*ndev);

	return 0;
}

int dtb_parse_uart(struct uart_device *uart)
{
	int node;
	const uint32_t *irq, *clock_freq;
	int len;
	uint64_t addr = 0;
	uint64_t size = 0;

	node = fdt_path_offset(dtb_virt, "/soc/serial");
	if (node < 0) {
		log_warn("UART node not found\n");
		return -EINVAL;
	}

	if (dtb_get_one_reg(node, &addr, &size)) {
		log_warn("UART reg property not found\n");
		return -EINVAL;
	}

	uart->phys_base = addr;
	uart->size = size;

	irq = fdt_getprop(dtb_virt, node, "interrupts", &len);
	if (!irq || len != sizeof(uint32_t)) {
		log_warn("UART interrupts property not found\n");
		return -EINVAL;
	}
	uart->irq = fdt32_to_cpu(*irq);

	clock_freq = fdt_getprop(dtb_virt, node, "clock-frequency", &len);
	if (!clock_freq || len != sizeof(uint32_t)) {
		log_warn("UART clock-frequency property not found\n");
		return -EINVAL;
	}
	uart->clock_freq = fdt32_to_cpu(*clock_freq);

	return 0;
}
