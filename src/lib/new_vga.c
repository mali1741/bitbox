/*

640 x 480 mode, 12 bits
- output is on PORTB, lower 12 bits
- sync is on PORT A, pin0 (vsync), pin1 (hsync)

*/

/* 
	PA1 (HSYNC) output is driven by Timer5 with CC 2 (see Table9 of datasheet, p60) using pwm mode
	Timer 1 (PIXEL DMA) is started as a slave of Timer5 CC1 via ITR1 (see config options table 76, p463 of refman)
   	TIM1_UP drives the DMA2 on stream 5 channel 6 (ref manual p164)
	DMA2 outputs to gpio PB0-15 ( refman p48 - DMA1 cannot drive AHB1 )
*/


/* debug with gdb : use 
	*(DMA_Stream_TypeDef*)0x40026488 (DMA2stream5 )
	*(DMA_TypeDef *)0x40026400  DMA2 status
	*(TIM_TypeDef*) 0x40010000 TIM1
	*(TIM_TypeDef*) 0x40000c00 TIM5
*/

#include "kernel.h" // base 

#include <stdint.h>

#include "system.h" // interrupts 
#include "stm32f4xx.h" // ports, timers, profile

#include "GPIO.h"
#include "RCC.h"

#include "../lib/kernel.h"

#ifdef GAMEPAD
#include "gamepad.h"
#endif 

#ifdef AUDIO
#include "audio.h"
#endif 


#ifdef PROFILE
// from http://forums.arm.com/index.php?/topic/13949-cycle-count-in-cortex-m3/
uint32_t line_time,max_line_time, max_line; // maximum time of line 
#endif 

#define PIXELCLOCK 7
#define MIN(x,y) ((x)<(y)?x:y) 


// public interface
uint32_t line;
volatile uint32_t frame; 

// aligned on a 1kb boundary , see http://blog.frankvh.com/2011/08/18/stm32f2xx-dma-controllers/
static pixel_t *LineBuffer1=(pixel_t *)0x20000000; // LineBuffer1[LINE_LENGTH]; <-- NO ?, would be in CCM so out of reach of DMA !
static pixel_t *LineBuffer2=(pixel_t *)0x20000000+1024;//!pixels !  +LINE_LENGTH*sizeof(pixel_t)+2*MARGIN; // LineBuffer2[LINE_LENGTH];

pixel_t *display_buffer; // will be sent to display 
pixel_t *draw_buffer; // will be drawn (bg already drawn)

static void HSYNCHandler();
static void DMACompleteHandler();


static inline void output_black()
{
	//GPIOB->BSRRH=0x0fff; // Set signal to black. 
	// GPIOB->BSRRH is at 0x4002 041A

	__asm__ volatile( // force do it NOW
	"   movw	r1,#0\n"
	"	movt	r1,#0x4002\n"
	"	movw 	r0,#0x0fff\n"
	"	strh	r0,[r1,#0x41A]\n"
	:::"r0","r1");
} 


void vga640_setup()
{
	//EnableAPB2PeripheralClock(RCC_APB2ENR_SYSCFGEN); // ??? useful ?

	// initialize software state.
	line=0;	frame=0;
	display_buffer=LineBuffer1;
	draw_buffer=LineBuffer2;

	// --- GPIO ---------------------------------------------------------------------------------------
	// init vga gpio ports A0 vsync, A1 hsync B0-11 dac
	// XXX here port PC11 is used for vsync !!!  

	// GPIO A pins 0 (vsync- not bitbox prototype) & 1 (hsync) and GPIO B for pixel DAC
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN; // enable gpioB

	// - Configure DAC pins in GPIOB 0-11
	SetGPIOOutputMode(GPIOB,0x0fff); 
	SetGPIOPushPullOutput(GPIOB,0x0fff);
	SetGPIOSpeed50MHz(GPIOB,0x0fff); 
	SetGPIOPullDownResistor(GPIOB,0x0fff);

	output_black();

	// - Configure sync pins as GPIOA 0 (vsync) , 1 (hsync)
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN; // enable gpioA
	SetGPIOAlternateFunctionMode(GPIOA,0b10); // PA1 as alternate
	SelectAlternateFunctionForGPIOPin(GPIOA,1,2); // TIM 5 CH 2, see Table9 of datasheet, p60 : alt func 2 is PA1

	SetGPIOOutputMode(GPIOA, 0b1); // PA0 as ouput

	SetGPIOPushPullOutput(GPIOA,0b11);
	SetGPIOSpeed50MHz(GPIOA,0b11);
	SetGPIOPullUpResistor(GPIOA,0b11);

	// Also set GPIOC as current bitbox has it 
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN; // enable gpioC
	SetGPIOOutputMode(GPIOC, 1<<11); // PC11 as ouput
	SetGPIOPushPullOutput(GPIOC,1<<11);
	SetGPIOSpeed50MHz(GPIOC,1<<11);
	SetGPIOPullUpResistor(GPIOC,1<<11);

	// drive them high
	GPIOA->BSRRL=0b11;

	// --- TIMERS ---------------------------------------------------------------------------------------
	
	// TIMER 5

	// -- Configure timer 5 as the HSync timer. Timer 5 runs at half frequency
	// CC2 is used to generate the HSync pulse, using PWM mode and driving the pin directly (channel 2 timer 5 drives gpio pin ..)
	// CC3 is used to generate a trigger signal for TIM1, which drives the pixel DMA, though ITR1
	// CC4 is used to trigger a soft interrupt
	
	// -- Global setup

	// enable timer 5
	RCC->APB1ENR |= RCC_APB1ENR_TIM5EN;

	TIM5->PSC=0; // XXX debug Prescaler = 1 : PSC=0
	TIM5->CR1=TIM_CR1_ARPE; // autoreload preload enable, no other function on
	TIM5->DIER=TIM_DIER_UIE; // Enable update interrupt.
	TIM5->CCER=0; // Disable CC, so we can program it.
	TIM5->ARR=2796-1; // 88 MHz (OC) / 31.46875 kHz = 2796.42502483 (88 = 176MHz / 2 )

	// -- Channel 2  : Hsync pulse

	// On CNT==0: sync pulse start
	// set to PWM mode active level on reload, passive level after CC2-match.
	TIM5->CCMR1=6*TIM_CCMR1_OC2M_0;
	// output enabled, reversed polarity (active low).
	TIM5->CCER=TIM_CCER_CC2E|TIM_CCER_CC2P; 
	// 88 MHz * 3.813 microseconds = 335.544 - sync pulse end
	TIM5->CCR2=336; 

	// -- Channel 3 : Trigger signal for TIM1 - will start on ITR1

	// Master mode selection OC3REF signal is used as trigger output (TRGO)
	TIM5->CR2=(0b110*TIM_CR2_MMS_0); 

	// Channel 3 set to passive level on reload, active level after CC3-match.
	TIM5->CCMR2=7*TIM_CCMR2_OC3M_0; 

	// 88 MHz * (3.813 + 1.907) microseconds = 503.36 - back porch end, start pixel clock
    // -14 is a kludge to account for slow start of timer.
	TIM5->CCR3=503-14;

	// Enable HSync timer.

	// -- Channel 4 : software Hsync interrupt
	// 88 MHz * (3.813 + 1.907) microseconds = 503.36 - back porch end, start pixel clock
	//TIM5->CCER=TIM_CCER_CC4E|TIM_CCER_CC4P; // Channel 4 enabled, reversed polarity (active low).
	TIM5->CCR4=503;
	// wait for last line ? 

	// Enable HSync timer interrupt and set highest priority.
	InstallInterruptHandler(TIM5_IRQn,HSYNCHandler);
	NVIC_EnableIRQ(TIM5_IRQn);
	NVIC_SetPriority(TIM5_IRQn,0);

	TIM5->CNT=-10; // Make sure it hits ARR. 
	TIM5->CR1|=TIM_CR1_CEN; // Go.


	// --- TIMER 1 : DMA2 pixel clock
	
	// enable it
	RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
	// Prescaler = 1.
	TIM1->PSC=0; 
	// loop each "pixelclock"
	TIM1->ARR=PIXELCLOCK-1; 
	// autoreload preload enable, no other function on
	TIM1->CR1=TIM_CR1_ARPE;
	// Enable update DMA request interrupt
	TIM1->DIER=TIM_DIER_UDE; 
	// Only run TIM1 when TIM5 trigger-out is high

	// SMS=5 : set Gated mode (clk enabled on input), 
	// TS: selection ITR0 : for timer 1 this is timer 5 (ref464)
	TIM1->SMCR=(5*TIM_SMCR_SMS_0)|(0*TIM_SMCR_TS_0); 

	// --- DMA -----------------------------------------------------------------------------------------

	// TIM1_UP drives the DMA2 on stream 5 channel 6 (ref. manual p168)
	
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN; // DMA2 enable

	// Stop it and configure interrupts.
	DMA2_Stream5->CR&=~DMA_SxCR_EN;
	
	NVIC_DisableIRQ(DMA2_Stream5_IRQn);
	InstallInterruptHandler(DMA2_Stream5_IRQn,DMACompleteHandler);
	NVIC_EnableIRQ(DMA2_Stream5_IRQn);
	NVIC_SetPriority(DMA2_Stream5_IRQn,0);


	#ifdef PROFILE
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk ; // enable the cycle counter
	#endif
}


static void prepare_pixel_DMA()
{
	// Visible line. Configure and enable pixel DMA.
	DMA2_Stream5->CR=(6*DMA_SxCR_CHSEL_0)| // Channel 6
	(3*DMA_SxCR_PL_0)| // Priority 3
	(1*DMA_SxCR_PSIZE_0)| // PSIZE = 16 bit
	(1*DMA_SxCR_MSIZE_0)| // MSIZE = 16 bit
	DMA_SxCR_MINC| // Increase memory address
	(1*DMA_SxCR_DIR_0)| // Memory to peripheral
	DMA_SxCR_TCIE |  // Transfer complete interrupt 
	(1*DMA_SxCR_MBURST_0); // burst on the memory-side

	DMA2_Stream5->NDTR=LINE_LENGTH+1; // transfer N pixels + one last _black_ pixel
	DMA2_Stream5->PAR=((uint32_t)&(GPIOB->ODR));
	DMA2_Stream5->M0AR=(uint32_t)display_buffer; // XXX +MARGIN*2;

	// Enable FIFO (see p190 of ref manual)
	// XXX DMA FIFO (as MBURST/ref p236) ?
	DMA2_Stream5->FCR |= DMA_SxFCR_DMDIS; 

	// Enable pixel clock. Clock will only start once TIM5 allows it.
	

	// Enable pixel clock. Clock will only start once TIM2 allows it.

	TIM1->DIER=0; // Update DMA request has to be disabled while zeroing the counter.
	TIM1->EGR=TIM_EGR_UG; // Force an update event to reset counter. Setting CNT is not reliable.
	TIM1->DIER=TIM_DIER_UDE; // Re-enable update DMA request.

	TIM1->CR1|=TIM_CR1_CEN; // go .. when slave active

	DMA2_Stream5->CR|=DMA_SxCR_EN; // Go .. when timer 3 will start.
}

static void HSYNCHandler()
{
	// TIM5->SR=0;
	__asm__ volatile(
	"	mov.w	r1,#0x40000000\n"
	"	movs	r0,#0\n"
	"	strh	r0,[r1,#0xC10]\n"
	:::"r0","r1");

	line++;
	if(line<480)
	{

		// swap display & draw buffers
		pixel_t *t;
		t=display_buffer;
		display_buffer = draw_buffer;
		draw_buffer = t;

		prepare_pixel_DMA(); // will be triggered 
		
		#ifdef PROFILE
		line_time = DWT->CYCCNT; // reset the perf counter
		#endif 
		
		game_line(); // Game callback !
		
		#ifdef PROFILE
		line_time = DWT->CYCCNT - line_time; // read the counter
		if (line_time>max_line_time) {
			max_line_time=line_time;
			max_line =line;
		}
		#endif
	}
	else {
		if (line<=480+33)
		{
			#ifdef GAMEPAD
			gamepad_readstep();
			#endif
		}

		if(line==480)
		{
			frame++; 
			// TODO : VBlank interrupt. waiting for frame can be enough.
		}
		else if(line==490)
		{
			//GPIOA->BSRRH|=(1<<0); // lower VSync line
			GPIOC->BSRRH|=(1<<11); // lower VSync line // THIS IS NOT REAL BITBOX !! only prototype
		}
		else if(line==492)
		{
			//GPIOA->BSRRL|=(1<<0); // raise VSync line
			GPIOC->BSRRL|=(1<<11); // raise VSync line
		}
		else if(line==524)
		{
			line=-1;
		}
	}

	#ifdef AUDIO
	game_sample(); 
	#endif
}

static void DMACompleteHandler()
{
	output_black(); // This should not be necessary, as timing is wrong in sw. 

	// Clear Transfer complete interrupt flag of stream 5
	do {
		DMA2->HIFCR|= DMA_HIFCR_CTCIF5;
	} while (DMA2->HISR & DMA_HISR_TCIF5);

	// stop DMA
	TIM1->CR1&=~TIM_CR1_CEN; // Stop pixel clock.
	DMA2_Stream5->CR=0; // Disable pixel DMA.
	// this will trigger a new interrupt. need to get rid of it !
	NVIC_ClearPendingIRQ (DMA2_Stream5_IRQn);    // clear pending DMA IRQ from the NVIC

}

