1. **Fix Timeout Issue:** The previous implementation caused a timeout (`-ETIMEDOUT` / `-110`) during probe. This happened because `REG_SPI_CTRL_MACMUX_SEL` was modified *before* switching the controller to manual mode. When in auto mode, the controller's Read FSM might be actively expecting data from the previously selected chip. Changing the multiplexer on the fly interrupts this stream, causing the wait for `SPI_CTRL_RDCTL_FSM` to become idle to timeout.
2. **Move MACMUX_SEL writes:** We must set `REG_SPI_CTRL_MACMUX_SEL` *after* calling `airoha_snand_set_mode` which gracefully turns off auto mode and waits for the FSM to become idle.
3. **Changes:**
   - Restore `airoha_snand_dirmap_read` to its previous state before `/* minimum oob size is 64 */`.
   - Add `regmap_write(as_ctrl->regmap_ctrl, REG_SPI_CTRL_MACMUX_SEL, spi_get_chipselect(desc->mem->spi, 0));` *after* `airoha_snand_set_mode(as_ctrl, SPI_MODE_DMA)`.
   - Do the same for `airoha_snand_dirmap_write`.
   - Do the same for `airoha_snand_exec_op` *after* `airoha_snand_set_mode(as_ctrl, SPI_MODE_MANUAL)`.
4. **Compile and verify.**
5. **Complete pre-commit steps.**
6. **Submit.**
