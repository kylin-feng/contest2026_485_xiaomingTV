/****************************************************************************
 * vendor/sifli/boards/sf32lb52/drivers/input/ft6146.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <syslog.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include <nuttx/input/touchscreen.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/wqueue.h>

#include "sifli_gpio.h"
#include "drv_io.h"

#include "ft6146.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FT6146_NAME_TOUCH     "input0"
#define FT6146_I2C_DEF_FREQ   400000
#define FT6146_DEV_ADDR        0x38

#define FT6146_REG_TD_STATUS   0x02
#define FT6146_REG_P1_XH       0x03
#define FT6146_REG_ID_H        0xa3
#define FT6146_REG_ID_L        0x9f

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ft6146_touch_lowerhalf_s
{
  struct touch_lowerhalf_s lower;
  struct i2c_master_s *i2c;
  struct work_s work;
  mutex_t devlock;
  sem_t waitsem;
  uint32_t pin_irq;
  uint16_t last_x;
  uint16_t last_y;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static ssize_t ft6146_touch_notify(FAR void *input_lower,
                                   FAR const char *buffer,
                                   size_t buflen);
static ssize_t ft6146_touch_write(FAR struct touch_lowerhalf_s *input_lower,
                                  FAR const char *buffer,
                                  size_t buflen);
static void ft6146_irq_handler(void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ft6146_i2c_read(struct i2c_master_s *i2c,
                           uint8_t reg, uint8_t *buf, uint8_t len)
{
  struct i2c_msg_s msgs[2];
  int ret;

  msgs[0].addr      = FT6146_DEV_ADDR;
  msgs[0].frequency = FT6146_I2C_DEF_FREQ;
  msgs[0].flags     = I2C_M_NOSTOP;
  msgs[0].buffer    = &reg;
  msgs[0].length    = 1;

  msgs[1].addr      = FT6146_DEV_ADDR;
  msgs[1].frequency = FT6146_I2C_DEF_FREQ;
  msgs[1].flags     = I2C_M_READ | I2C_M_NOSTART;
  msgs[1].buffer    = buf;
  msgs[1].length    = len;

  ret = I2C_TRANSFER(i2c, msgs, 2);
  return ret < 0 ? ret : OK;
}

static void ft6146_data_worker(void *param)
{
  struct ft6146_touch_lowerhalf_s *ft6146 =
    (struct ft6146_touch_lowerhalf_s *)param;
  struct touch_sample_s sample;
  uint8_t buf[5] = {0};
  uint8_t touch_num;
  int ret;

  ret = ft6146_i2c_read(ft6146->i2c, FT6146_REG_TD_STATUS, buf, sizeof(buf));
  if (ret < 0)
    {
      ierr("ft6146: read touch data failed: %d\n", ret);
      return;
    }

  touch_num = buf[0] & 0x0f;

  sample.npoints = 1;
  if (touch_num > 0)
    {
      ft6146->last_x = ((uint16_t)(buf[1] & 0x0f) << 8) | buf[2];
      ft6146->last_y = ((uint16_t)(buf[3] & 0x0f) << 8) | buf[4];
      sample.point[0].flags = TOUCH_DOWN;
    }
  else
    {
      sample.point[0].flags = TOUCH_UP;
    }

  sample.point[0].x = ft6146->last_x;
  sample.point[0].y = ft6146->last_y;

  touch_event(ft6146->lower.priv, &sample);
}

static void ft6146_irq_handler(void *arg)
{
  struct ft6146_touch_lowerhalf_s *ft6146 =
    (struct ft6146_touch_lowerhalf_s *)arg;
  int ret;

  ret = work_queue(HPWORK, &ft6146->work, ft6146_data_worker, ft6146, 0);
  DEBUGASSERT(ret == OK);
}

static int ft6146_hw_init(struct ft6146_touch_lowerhalf_s *ft6146)
{
  uint8_t id_h = 0;
  uint8_t id_l = 0;
  int ret;

  BSP_TP_PowerUp();
  BSP_TP_Reset(0);
  usleep(5000);
  BSP_TP_Reset(1);
  usleep(80000);

  /* Always-on bring-up diagnostics.  ierr() expands to nothing unless
   * CONFIG_DEBUG_INPUT_ERROR is set, which is why the previous boot log only
   * showed the outer -5 with no clue which step produced it.
   */
  syslog(LOG_ERR, "[ft6146][diag] irq_pin=%lu addr=0x%02x freq=%d\n",
         (unsigned long)ft6146->pin_irq, FT6146_DEV_ADDR, FT6146_I2C_DEF_FREQ);

  /* Probe every valid 7-bit address now that the controller is out of reset.
   * A hit at 0x38 proves the touch chip and its FPC are alive, so any later
   * failure is a driver/config problem instead of a wiring problem.
   */
  {
    int ndev = 0;
    unsigned int ia;
    for (ia = 0x08; ia <= 0x77; ia++)
      {
        struct i2c_msg_s probe;
        uint8_t dummy = 0;
        probe.addr      = (uint16_t)ia;
        probe.frequency = FT6146_I2C_DEF_FREQ;
        probe.flags     = I2C_M_READ;
        probe.buffer    = &dummy;
        probe.length    = 1;
        if (I2C_TRANSFER(ft6146->i2c, &probe, 1) >= 0)
          {
            syslog(LOG_ERR, "[ft6146][diag] i2c ACK from 0x%02x\n", ia);
            ndev++;
          }
      }
    syslog(LOG_ERR, "[ft6146][diag] i2c scan done: %d device(s), "
                    "FT6146 expected at 0x%02x\n", ndev, FT6146_DEV_ADDR);
  }

  ret = sifli_gpio_config(ft6146->pin_irq, GPIO_INPUT);
  syslog(LOG_ERR, "[ft6146][diag] gpio_config(irq) ret=%d\n", ret);
  if (ret < 0)
    {
      ierr("ft6146: irq pin config failed: %d\n", ret);
      return ret;
    }

  ret = ft6146_i2c_read(ft6146->i2c, FT6146_REG_ID_H, &id_h, 1);
  syslog(LOG_ERR, "[ft6146][diag] read ID_H(0x%02x) ret=%d val=0x%02x\n",
         FT6146_REG_ID_H, ret, id_h);
  if (ret < 0)
    {
      ierr("ft6146: read id_h failed: %d\n", ret);
      return ret;
    }

  ret = ft6146_i2c_read(ft6146->i2c, FT6146_REG_ID_L, &id_l, 1);
  syslog(LOG_ERR, "[ft6146][diag] read ID_L(0x%02x) ret=%d val=0x%02x\n",
         FT6146_REG_ID_L, ret, id_l);
  if (ret < 0)
    {
      ierr("ft6146: read id_l failed: %d\n", ret);
      return ret;
    }

  iinfo("ft6146 id_h=0x%02x id_l=0x%02x\n", id_h, id_l);

  ret = sifli_gpio_set_event(ft6146->pin_irq, false, true,
                             ft6146_irq_handler, ft6146);
  syslog(LOG_ERR, "[ft6146][diag] gpio_set_event ret=%d\n", ret);
  if (ret < 0)
    {
      ierr("ft6146: irq event setup failed: %d\n", ret);
      return ret;
    }

  return OK;
}

static ssize_t ft6146_touch_notify(FAR void *input_lower,
                                   FAR const char *buffer,
                                   size_t buflen)
{
  FAR struct ft6146_touch_lowerhalf_s *ft6146 =
    (FAR struct ft6146_touch_lowerhalf_s *)input_lower;
  FAR const struct touch_sample_s *sample =
    (FAR const struct touch_sample_s *)buffer;

  touch_event(ft6146->lower.priv, sample);
  return buflen;
}

static ssize_t ft6146_touch_write(FAR struct touch_lowerhalf_s *input_lower,
                                  FAR const char *buffer,
                                  size_t buflen)
{
  FAR struct ft6146_touch_lowerhalf_s *ft6146 =
    (FAR struct ft6146_touch_lowerhalf_s *)input_lower;

  return ft6146_touch_notify(ft6146, buffer, buflen);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ft6146_touch_initialize(struct i2c_master_s *i2c, uint32_t irq_pin)
{
  FAR struct ft6146_touch_lowerhalf_s *ft6146;
  int ret;

  DEBUGASSERT(i2c != NULL);

  ft6146 = kmm_zalloc(sizeof(struct ft6146_touch_lowerhalf_s));
  if (ft6146 == NULL)
    {
      return -ENOMEM;
    }

  ft6146->lower.write = ft6146_touch_write;
  ft6146->lower.maxpoint = 1;
  ft6146->i2c = i2c;
  ft6146->pin_irq = irq_pin;

  nxmutex_init(&ft6146->devlock);
  nxsem_init(&ft6146->waitsem, 0, 0);

  ret = touch_register(&ft6146->lower, "/dev/" FT6146_NAME_TOUCH, 16);
  syslog(LOG_ERR, "[ft6146][diag] touch_register ret=%d\n", ret);
  if (ret < 0)
    {
      nxmutex_destroy(&ft6146->devlock);
      nxsem_destroy(&ft6146->waitsem);
      kmm_free(ft6146);
      return ret;
    }

  ret = ft6146_hw_init(ft6146);
  if (ret < 0)
    {
      touch_unregister(&ft6146->lower, "/dev/" FT6146_NAME_TOUCH);
      nxmutex_destroy(&ft6146->devlock);
      nxsem_destroy(&ft6146->waitsem);
      kmm_free(ft6146);
      return ret;
    }

  return OK;
}
