#pragma once

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32S3)

extern "C"
{
#include <driver/periph_ctrl.h>
#include <rom/gpio.h>
#include <hal/dma_types.h>
#include <hal/gpio_hal.h>
#include <soc/lcd_cam_struct.h>
}

// ESP32 Endian Map
// uint16_t
//   1234
//   3412
// uint32_t
//   12345678
//   78563412
// uint64_t
//   0123456789abcdef
//   efcdab8967452301

class NeoEspLcdMuxBusSize8Bit
{
public:
    NeoEspLcdMuxBusSize8Bit() {};

    const static size_t MuxBusDataSize = 1;

    static void EncodeIntoDma(uint8_t** dmaBuffer, const uint8_t* data, size_t sizeData, uint8_t muxId)
    {
#if defined(CONFIG_IDF_TARGET_ESP32S2)
        // 1234  - order
        // 3412  = actual due to endianness
        //                                00000001
        const uint32_t EncodedZeroBit = 0x00000100;
        //                                00010101
        const uint32_t EncodedOneBit =  0x01000101;
#else
        //  8 channel bits layout for DMA 32bit value
        //  note, right to left
        //  mux bus bit/id     76543210 76543210 76543210 76543210
        //  encode bit #       3        2        1        0
        //  value zero         0        0        0        1
        //  value one          0        1        1        1    
        //
        // due to indianness between peripheral and cpu, bytes within the words are swapped in the const
        // 1234  - order
        // 3412  = actual due to endianness
        //                                00000001
        const uint32_t EncodedZeroBit = 0x00010000;
        //                               00010101
        const uint32_t EncodedOneBit = 0x01010001;
#endif

        uint32_t* pDma = reinterpret_cast<uint32_t*>(*dmaBuffer);
        const uint8_t* pEnd = data + sizeData;

        for (const uint8_t* pPixel = data; pPixel < pEnd; pPixel++)
        {
            uint8_t value = *pPixel;

            for (uint8_t bit = 0; bit < 8; bit++)
            {
                uint32_t dma = *(pDma);

                dma |= (((value & 0x80) ? EncodedOneBit : EncodedZeroBit) << (muxId));
                *(pDma++) = dma;
                value <<= 1;
            }
        }
        // return the buffer pointer advanced by encoding progress
        *dmaBuffer = reinterpret_cast<uint8_t*>(pDma);
    }
};

//
// tracks mux channels used and if updated
// 
// T_FLAG - type used to store bit flags, UINT16_t for 16 channels
// T_MUXSIZE - true size of mux channel = NeoEspLcdMuxBusSize8Bit
//
template<typename T_FLAG, typename T_MUXSIZE> 
class NeoEspLcdMuxMap : public T_MUXSIZE
{
public:
    const static uint8_t InvalidMuxId = -1;
    const static size_t BusMaxCount = sizeof(T_FLAG) * 8;

    size_t MaxBusDataSize; // max size of stream data from any single mux bus
    T_FLAG UpdateMap;     // bitmap flags of mux buses to track update state
    T_FLAG UpdateMapMask; // mask to used bits in s_UpdateMap
    T_FLAG BusCount;      // count of mux buses

    // as a static instance, all members get initialized to zero
    // and the constructor is called at inconsistent time to other globals
    // so its not useful to have or rely on, 
    // but without it presence they get zeroed far too late
    NeoEspLcdMuxMap() 
    //    //:
    //    //MaxBusDataSize(0),
    //    //UpdateMap(0),
    //    //UpdateMapMask(0),
    //    //BusCount(0)
    {
    }

    uint8_t RegisterNewMuxBus(const size_t dataSize)
    {
        // find first available bus id
        uint8_t muxId = 0;
        while (muxId < BusMaxCount)
        {
            T_FLAG muxIdField = (1 << muxId);
            if ((UpdateMapMask & muxIdField) == 0)
            {
                // complete registration
                BusCount++;
                UpdateMapMask |= muxIdField;
                if (dataSize > MaxBusDataSize)
                {
                    MaxBusDataSize = dataSize;
                }
                break;
            }
            muxId++;
        }
        if (muxId == BusMaxCount)
        {
            log_e("exceded channel limit of %u on bus", BusMaxCount);
        }
        return muxId;
    }


    bool DeregisterMuxBus(uint8_t muxId)
    {
        T_FLAG muxIdField = (1 << muxId);
        if (UpdateMapMask & muxIdField)
        {
            // complete deregistration
            BusCount--;
            UpdateMapMask &= ~muxIdField;
            if (UpdateMapMask == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool IsAllMuxBusesUpdated()
    {
        return (UpdateMap == UpdateMapMask);
    }

    bool IsNoMuxBusesUpdate()
    {
        return (UpdateMap == 0);
    }

    void MarkMuxBusUpdated(uint8_t muxId)
    {
        UpdateMap |= (1 << muxId);
    }

    void ResetMuxBusesUpdated()
    {
        UpdateMap = 0;
    }

    void Reset()
    {
        MaxBusDataSize = 0;
        UpdateMap = 0;
        UpdateMapMask = 0;
        BusCount = 0;
    }
};

//
// Implementation of a Single Buffered version of a LcdContext
// Manages the underlying I2S details including the buffer
// This creates only a actively sending back buffer, 
// Note that the back buffer must be DMA memory, a limited resource
// 
// T_MUXMAP - NeoEspLcdMuxMap - tracking class for mux state
//
template<typename T_MUXMAP> 
class NeoEspLcdMonoBuffContext 
{
public:
    const static size_t DmaBitsPerPixelBit = 4;

    size_t LcdBufferSize; // total size of LcdBuffer
    uint8_t* LcdBuffer;    // holds the DMA buffer that is referenced by LcdBufDesc
    T_MUXMAP MuxMap;

    // as a static instance, all members get initialized to zero
    // and the constructor is called at inconsistent time to other globals
    // so its not useful to have or rely on, 
    // but without it presence they get zeroed far too late
    NeoEspLcdMonoBuffContext()
        //:
        //LcdBufferSize(0),
        //LcdBuffer(nullptr),
        //LcdEditBuffer(nullptr),
        //MuxMap()
    {
    }

    void Construct(const uint8_t busNumber)
    {
        // construct only once on first time called
        if (LcdBuffer == nullptr)
        {
//             // MuxMap.MaxBusDataSize = max size in bytes of a single channel
//             // DmaBitsPerPixelBit = how many dma bits/byte are needed for each source (pixel) bit/byte
//             // T_MUXMAP::MuxBusDataSize = the true size of data for selected mux mode (not exposed size as i2s0 only supports 16bit mode)
//             LcdBufferSize = MuxMap.MaxBusDataSize * 8 * DmaBitsPerPixelBit * T_MUXMAP::MuxBusDataSize;

            uint16_t numLEDs = MuxMap.MaxBusDataSize * 8; // (total, all strips) TODO: dont hardcode the 8, but its inconsistent between 8 and 16 bit classes?
            uint8_t bytesPerPixel = 3; // TODO: dont hardcode, 3 for RGB, 4 for RGBW

            // COLIN TODO: why 3 everywhere? is that the bytesPerPixel value?
            uint32_t xfer_size = numLEDs * bytesPerPixel * 3;
            uint32_t buf_size = xfer_size + 3;        // +3 for long align
            int num_desc = (xfer_size + 4094) / 4095; // sic. (NOT 4096)
            uint32_t alloc_size =
                num_desc * sizeof(dma_descriptor_t) + (/*dbuf ? buf_size * 2 : */buf_size);

            LcdBufferSize = alloc_size;

//             // must have a 4 byte aligned buffer for i2s
//             uint32_t alignment = LcdBufferSize % 4;
//             if (alignment)
//             {
//                 LcdBufferSize += 4 - alignment;
//             }

//             size_t dmaBlockCount = (LcdBufferSize + I2S_DMA_MAX_DATA_LEN - 1) / I2S_DMA_MAX_DATA_LEN;

            LcdBuffer = static_cast<uint8_t*>(heap_caps_malloc(LcdBufferSize, MALLOC_CAP_DMA));
            if (LcdBuffer == nullptr)
            {
                log_e("send buffer memory allocation failure (size %u)",
                    LcdBufferSize);
            }
            memset(LcdBuffer, 0x00, LcdBufferSize);

            // Find first 32-bit aligned address following descriptor list
            uint32_t *alignedAddr =
                (uint32_t *)((uint32_t)(&LcdBuffer[num_desc * sizeof(dma_descriptor_t) + 3]) & ~3);
            uint8_t *dmaBuf = (uint8_t *)alignedAddr[0];

            // Colin note: LcdBuffer is no longer just pixel data, it's also got descriptors
            // DMA buffer starts at dmaBuf not at LcdBuffer

//             i2sInit(busNumber,
//                 true,
//                 T_MUXMAP::MuxBusDataSize,
//                 i2sSampleRate,
// #if defined(CONFIG_IDF_TARGET_ESP32S2)
// // using these modes on ESP32S2 actually allows it to function
// // in both x8 and x16
//                 I2S_CHAN_STEREO,
//                 I2S_FIFO_16BIT_DUAL,
// #else
// // but they won't work on ESP32 in parallel mode, but these will
//                 I2S_CHAN_RIGHT_TO_LEFT,
//                 I2S_FIFO_16BIT_SINGLE,
// #endif
//                 dmaBlockCount,
//                 LcdBuffer,
//                 LcdBufferSize);

            // LCD_CAM isn't enabled by default -- MUST begin with this:
            periph_module_enable(PERIPH_LCD_CAM_MODULE);
            periph_module_reset(PERIPH_LCD_CAM_MODULE);

            // Reset LCD bus
            LCD_CAM.lcd_user.lcd_reset = 1;
            esp_rom_delay_us(100);

            // Configure LCD clock
            LCD_CAM.lcd_clock.clk_en = 1;             // Enable clock
            LCD_CAM.lcd_clock.lcd_clk_sel = 2;        // PLL240M source
            LCD_CAM.lcd_clock.lcd_clkm_div_a = 1;     // 1/1 fractional divide,
            LCD_CAM.lcd_clock.lcd_clkm_div_b = 1;     // plus '99' below yields...
            LCD_CAM.lcd_clock.lcd_clkm_div_num = 99;  // 1:100 prescale (2.4 MHz CLK)
            LCD_CAM.lcd_clock.lcd_ck_out_edge = 0;    // PCLK low in 1st half cycle
            LCD_CAM.lcd_clock.lcd_ck_idle_edge = 0;   // PCLK low idle
            LCD_CAM.lcd_clock.lcd_clk_equ_sysclk = 1; // PCLK = CLK (ignore CLKCNT_N)

            // Configure frame format
            LCD_CAM.lcd_ctrl.lcd_rgb_mode_en = 0;    // i8080 mode (not RGB)
            LCD_CAM.lcd_rgb_yuv.lcd_conv_bypass = 0; // Disable RGB/YUV converter
            LCD_CAM.lcd_misc.lcd_next_frame_en = 0;  // Do NOT auto-frame
            LCD_CAM.lcd_data_dout_mode.val = 0;      // No data delays
            LCD_CAM.lcd_user.lcd_always_out_en = 1;  // Enable 'always out' mode
            LCD_CAM.lcd_user.lcd_8bits_order = 0;    // Do not swap bytes
            LCD_CAM.lcd_user.lcd_bit_order = 0;      // Do not reverse bit order
            LCD_CAM.lcd_user.lcd_2byte_en = 0;       // 8-bit data mode
            LCD_CAM.lcd_user.lcd_dummy = 1;          // Dummy phase(s) @ LCD start
            LCD_CAM.lcd_user.lcd_dummy_cyclelen = 0; // 1 dummy phase
            LCD_CAM.lcd_user.lcd_cmd = 0;            // No command at LCD start
            // Dummy phase(s) MUST be enabled for DMA to trigger reliably.

            // Colin TODO (pin stuff goes elsewhere)
            // const uint8_t mux[] = {
            //     LCD_DATA_OUT0_IDX, LCD_DATA_OUT1_IDX, LCD_DATA_OUT2_IDX,
            //     LCD_DATA_OUT3_IDX, LCD_DATA_OUT4_IDX, LCD_DATA_OUT5_IDX,
            //     LCD_DATA_OUT6_IDX, LCD_DATA_OUT7_IDX,
            // };

            // // Route LCD signals to GPIO pins
            // for (int i = 0; i < 8; i++) {
            //     if (pins[i] >= 0) {
            //     esp_rom_gpio_connect_out_signal(pins[i], mux[i], false, false);
            //     gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[pins[i]], PIN_FUNC_GPIO);
            //     gpio_set_drive_capability((gpio_num_t)pins[i], (gpio_drive_cap_t)3);
            //     bitmask[i] = 1 << i;
            // }

            // Set up DMA descriptor list (length and data are set before xfer)
            desc = (dma_descriptor_t *)LcdBuffer; // At start of alloc'd buffer
            for (int i = 0; i < num_desc; i++) {
                desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
                desc[i].dw0.suc_eof = 0;
                desc[i].next = &desc[i + 1];
            }
            desc[num_desc - 1].dw0.suc_eof = 1;
            desc[num_desc - 1].next = NULL;

            // Colin TODO
        }
    }

    void Destruct(const uint8_t busNumber)
    {
        if (LcdBuffer == nullptr)
        {
            return;
        }

        // i2sSetPins(busNumber, -1, -1, -1, false);
        // i2sDeinit(busNumber);

        heap_caps_free(LcdBuffer);

        LcdBufferSize = 0;
        LcdBuffer = nullptr;

        MuxMap.Reset();
    }

    void ResetBuffer()
    {
        // to keep the inner loops for EncodeIntoDma smaller
        // they will just OR in their values
        // so the buffer must be cleared first
        if (MuxMap.IsNoMuxBusesUpdate())
        {
            // clear all the data in preperation for each mux channel to add
            memset(LcdBuffer, 0x00, LcdBufferSize);
        }
    }

    void FillBuffer(uint8_t** dmaBuffer,
            const uint8_t* data, 
            size_t sizeData, 
            uint8_t muxId)
    {
        MuxMap.EncodeIntoDma(dmaBuffer,
            data,
            sizeData,
            muxId);
    }


    void StartWrite(uint8_t i2sBusNumber)
    {
        if (MuxMap.IsAllMuxBusesUpdated())
        {
            MuxMap.ResetMuxBusesUpdated();
            // i2sWrite(i2sBusNumber);
        }
    }
};


//
// Implementation of the low level interface into i2s mux bus
// 
// T_BUSCONTEXT - the context to use, currently only NeoEspLcdMonoBuffContext
//
template<typename T_BUSCONTEXT> 
class NeoEsp32LcdMuxBus
{
public:    
    NeoEsp32LcdMuxBus() :
        _muxId(s_context.MuxMap.InvalidMuxId)
    {
    }

    void RegisterNewMuxBus(size_t dataSize)
    {
        _muxId = s_context.MuxMap.RegisterNewMuxBus(dataSize);
    }

    void Initialize(uint8_t pin)
    {
        s_context.Construct(0);
        //i2sSetPins(T_BUS::LcdBusNumber, pin, _muxId, s_context.MuxMap.MuxBusDataSize, invert);
        // TODO: lcd set pins?
    }

    void DeregisterMuxBus(uint8_t pin)
    {
        if (s_context.MuxMap.DeregisterMuxBus(_muxId))
        {
            s_context.Destruct(0);
        }

        // disconnect muxed pin
        gpio_matrix_out(pin, SIG_GPIO_OUT_IDX, false, false);
        pinMode(pin, INPUT);

        _muxId = s_context.MuxMap.InvalidMuxId;
    }

    bool IsWriteDone() const
    {
        // return i2sWriteDone(0);
        return true;
        // TODO
    }

    uint8_t* BeginUpdate()
    {
        s_context.ResetBuffer();
        return s_context.LcdBuffer;
    }

    void FillBuffer(uint8_t** dmaBuffer,
        const uint8_t* data,
        size_t sizeData)
    {
        s_context.FillBuffer(dmaBuffer, data, sizeData, _muxId);
    }

    void EndUpdate()
    {
        s_context.MuxMap.MarkMuxBusUpdated(_muxId);
        s_context.StartWrite(0); // only when all buses are update is actual write started
    }

private:
    static T_BUSCONTEXT s_context;
    uint8_t _muxId; 
};

template<typename T_BUSCONTEXT> T_BUSCONTEXT NeoEsp32LcdMuxBus<T_BUSCONTEXT>::s_context = T_BUSCONTEXT();





//
// wrapping layer of the lcd mux bus as a NeoMethod
// 
// T_SPEED - NeoEsp32LcdSpeed* (ex NeoEsp32LcdSpeedWs2812x) used to define output signal form
// T_BUS - NeoEsp32LcdMuxBus, the bus to use
//
template<typename T_SPEED, typename T_BUS> 
class NeoEsp32LcdXMethodBase
{
public:
    typedef NeoNoSettings SettingsObject;

    NeoEsp32LcdXMethodBase(uint8_t pin, uint16_t pixelCount, size_t elementSize, size_t settingsSize) :
        _pin(pin),
        _pixelCount(pixelCount),
        _bus()
    {
        _bus.RegisterNewMuxBus((pixelCount * elementSize + settingsSize) + T_SPEED::ResetTimeUs / T_SPEED::ByteSendTimeUs);
    }

    ~NeoEsp32LcdXMethodBase()
    {
        while (!_bus.IsWriteDone())
        {
            yield();
        }

        _bus.DeregisterMuxBus(_pin);
    }

    bool IsReadyToUpdate() const
    {
        return _bus.IsWriteDone();
    }

    void Initialize()
    {
        _bus.Initialize(_pin);
    }

    template <typename T_COLOR_OBJECT,
        typename T_COLOR_FEATURE,
        typename T_SHADER>
    void Update(
        T_COLOR_OBJECT* pixels,
        size_t countPixels,
        const typename T_COLOR_FEATURE::SettingsObject& featureSettings,
        const T_SHADER& shader)
    {
        // wait for not actively sending data
        while (!_bus.IsWriteDone())
        {
            yield();
        }

        const size_t sendDataSize = T_COLOR_FEATURE::SettingsSize >= T_COLOR_FEATURE::PixelSize ? T_COLOR_FEATURE::SettingsSize : T_COLOR_FEATURE::PixelSize;
        uint8_t sendData[sendDataSize];
        uint8_t* data = _bus.BeginUpdate();

        // if there are settings at the front
        //
        if (T_COLOR_FEATURE::applyFrontSettings(sendData, sendDataSize, featureSettings))
        {
            _bus.FillBuffer(&data, sendData, T_COLOR_FEATURE::SettingsSize);
        }

        // apply primary color data
        //
        T_COLOR_OBJECT* pixel = pixels;
        const T_COLOR_OBJECT* pixelEnd = pixel + countPixels;
        uint16_t stripCount = _pixelCount;

        while (stripCount--)
        {
            typename T_COLOR_FEATURE::ColorObject color = shader.Apply(*pixel);
            T_COLOR_FEATURE::applyPixelColor(sendData, sendDataSize, color);

            _bus.FillBuffer(&data, sendData, T_COLOR_FEATURE::PixelSize);

            pixel++;

            if (pixel >= pixelEnd)
            {
                // restart at first
                pixel = pixels;
            }
        }


        // if there are settings at the back
        //
        if (T_COLOR_FEATURE::applyBackSettings(sendData, sendDataSize, featureSettings))
        {
            _bus.FillBuffer(&data, sendData, T_COLOR_FEATURE::SettingsSize);
        }

        _bus.EndUpdate(); // triggers actual write after all mux busses have updated
    }

    void applySettings([[maybe_unused]] const SettingsObject& settings)
    {
    }

private:
    const uint8_t _pin;            // output pin number
    const uint16_t _pixelCount; // count of pixels in the strip

    T_BUS _bus;          // holds instance for mux bus support
};













typedef NeoEsp32LcdMuxBus<NeoEspLcdMonoBuffContext<NeoEspLcdMuxMap<uint8_t, NeoEspLcdMuxBusSize8Bit>>> NeoEsp32LcdMux8Bus;

class NeoEsp32LcdSpeedWs2812x
{
public:
    const static uint16_t ByteSendTimeUs = 10;
    const static uint16_t ResetTimeUs = 300;
};

typedef NeoEsp32LcdXMethodBase<NeoEsp32LcdSpeedWs2812x, NeoEsp32LcdMux8Bus> NeoEsp32LcdX8Ws2812xMethod;


#endif // defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32S3)
