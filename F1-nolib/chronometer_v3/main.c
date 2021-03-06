/*
 * main.c
 *
 * Copyright 2017 Edward V. Emelianoff <eddy@sao.ru, edward.emelianoff@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include "adc.h"
#include "GPS.h"
#include "flash.h"
#include "fonts.h"
#include "hardware.h"
#include "lidar.h"
#include "screen.h"
#include "spi.h"
#include "str.h"
#include "time.h"
#include "usart.h"
#include "usb.h"
#include "usb_lib.h"

#ifndef VERSION
#define VERSION "0.0.0"
#endif

// global pseudo-milliseconds counter
volatile uint32_t Tms = 0;

/* Called when systick fires */
void sys_tick_handler(void){
    ++Tms; // increment pseudo-milliseconds counter
    if(++Timer == 1000){ // increment milliseconds counter
        time_increment();
    }
}

void iwdg_setup(){
    uint32_t tmout = 16000000;
    /* Enable the peripheral clock RTC */
    /* (1) Enable the LSI (40kHz) */
    /* (2) Wait while it is not ready */
    RCC->CSR |= RCC_CSR_LSION; /* (1) */
    while((RCC->CSR & RCC_CSR_LSIRDY) != RCC_CSR_LSIRDY){if(--tmout == 0) break;} /* (2) */
    /* Configure IWDG */
    /* (1) Activate IWDG (not needed if done in option bytes) */
    /* (2) Enable write access to IWDG registers */
    /* (3) Set prescaler by 64 (1.6ms for each tick) */
    /* (4) Set reload value to have a rollover each 2s */
    /* (5) Check if flags are reset */
    /* (6) Refresh counter */
    IWDG->KR = IWDG_START; /* (1) */
    IWDG->KR = IWDG_WRITE_ACCESS; /* (2) */
    IWDG->PR = IWDG_PR_PR_1; /* (3) */
    IWDG->RLR = 1250; /* (4) */
    tmout = 16000000;
    while(IWDG->SR){if(--tmout == 0) break;} /* (5) */
    IWDG->KR = IWDG_REFRESH; /* (6) */
}

#define USBBUF 63
// usb getline
static char *get_USB(){
    static char tmpbuf[USBBUF+1], *curptr = tmpbuf;
    static int rest = USBBUF;
    int x = USB_receive(curptr, rest);
    if(!x) return NULL;
    curptr[x] = 0;
    if(x == 1 && *curptr == 0x7f){ // backspace
        if(curptr > tmpbuf){
            --curptr;
            USB_send("\b \b");
        }
        return NULL;
    }
    USB_send(curptr); // echo
    if(curptr[x-1] == '\n'){ // || curptr[x-1] == '\r'){
        curptr = tmpbuf;
        rest = USBBUF;
        // omit empty lines
        if(tmpbuf[0] == '\n') return NULL;
        // and wrong empty lines
        if(tmpbuf[0] == '\r' && tmpbuf[1] == '\n') return NULL;
        return tmpbuf;
    }
    curptr += x; rest -= x;
    if(rest <= 0){ // buffer overflow
        sendstring("\nUSB buffer overflow!\n");
        curptr = tmpbuf;
        rest = USBBUF;
    }
    return NULL;
}

void linecoding_handler(usb_LineCoding __attribute__((unused)) *lc){ // get/set line coding
#ifdef EBUG
    SEND("Change speed to ");
    printu(1, lc->dwDTERate);
    newline(1);
#endif
}


static volatile uint8_t USBconn = 0;
uint8_t USB_connected = 0; // need for usb.c
void clstate_handler(uint16_t __attribute__((unused)) val){ // lesser bits of val: RTS|DTR
    USBconn = 1; // if == 1 -> send welcome message
    USB_connected = 1;
#if 0
    if(val & 2){
        DBG("RTS set");
        sendstring("RTS set\n");
    }
    if(val & 1){
        DBG("DTR set");
        sendstring("DTR set\n");
    }
#endif
}

void break_handler(){ // client disconnected
    DBG("Disconnected");
    USB_connected = 0;
}

int main(void){
    uint32_t lastT = 0, lastTshow = 0; // last time of status check, last time of time displayed
    uint8_t evtDisp = 0; // last event was displayed
    sysreset();
    StartHSE();
    SysTick_Config(SYSTICK_DEFCONF); // function SysTick_Config decrements argument!
    // read data stored in flash - before all pins/ports setup!!!
    flashstorage_init();
    // !!! hw_setup() should be the first in setup stage
    hw_setup();
    USB_setup();
    USBPU_ON();
#ifdef EBUG
    SEND("This is chronometer version " VERSION ".\n");
    if(RCC->CSR & RCC_CSR_IWDGRSTF){ // watchdog reset occured
        SEND("WDGRESET=1\n");
    }
    if(RCC->CSR & RCC_CSR_SFTRSTF){ // software reset occured
        SEND("SOFTRESET=1\n");
    }
#endif
    RCC->CSR |= RCC_CSR_RMVF; // remove reset flags
#ifdef EBUG
    usarts_setup(); // setup usarts for debug console
#endif
    usarts_setup(); // setup usarts after reading configuration
    spi_setup();
    ScreenOFF(); // clear screen
    PutStringAt(0, SCREEN_HEIGHT-1-curfont->baseline, "Chrono");
    ConvertScreenBuf();
    ShowScreen();
    iwdg_setup();

    while (1){
        IWDG->KR = IWDG_REFRESH; // refresh watchdog
        if(Timer > 499) LED_on(); // turn ON LED0 over 0.25s after PPS pulse
        if(USBconn && Tms > 100){ // USB connection
            USBconn = 0;
            sendstring("Chronometer version " VERSION ".\n");
        }
        // check if triggers that was recently shot are off now
        fillunshotms();
        if(Tms - lastT > 499){
            if(need2startseq) GPS_send_start_seq();
            IWDG->KR = IWDG_REFRESH;
            switch(GPS_status){
                case GPS_VALID:
                    LED1_blink(); // blink LED1 @ VALID time
                break;
                case GPS_NOT_VALID:
                    LED1_on(); // shine LED1 @ NON-VALID time
                break;
                default:
                    LED1_off(); // turn off LED1 if GPS not found or time unknown
            }
            lastT = Tms;
            IWDG->KR = IWDG_REFRESH;
            transmit_tbuf(1); // non-blocking transmission of data from UART buffer every 0.5s
            transmit_tbuf(GPS_USART);
            transmit_tbuf(LIDAR_USART);
#if 0
#ifdef EBUG
            static int32_t oldctr = 0;
            if(timecntr && timecntr != oldctr){
                oldctr = timecntr;
                SEND("ticksdiff=");
                if(ticksdiff < 0){
                    SEND("-");
                    printu(1, -ticksdiff);
                }else printu(1, ticksdiff);
                SEND(", timecntr=");
                printu(1, timecntr);
                SEND("\nlast_corr_time=");
                printu(1, last_corr_time);
                SEND(", Tms=");
                printu(1, Tms1);
                SEND("\nTimer=");
                printu(1, timerval);
                SEND(", LOAD=");
                printu(1, SysTick->LOAD);
                usart_putchar(1, '\n');
                newline(1);
            }
#endif
#endif
        }
        IWDG->KR = IWDG_REFRESH;
        if(showtime){ // show current time (HH:MM:SS.S) every 100ms
            if(Tms - lastTtrig < the_conf.ledshow_time && showshtr){ // show last event on screen
                if(!evtDisp){
                    evtDisp = 1;
                    FillScreen(0);
                    choose_font(FONTN10);
                    char *tm = get_scrntime(&lastLog.shottime.Time, lastLog.shottime.millis);
                    tm[11] = 0; // throw out thousands
                    uint16_t w = GetStrWidth(tm);
                    PutStringAt((SCREEN_WIDTH-w-1)/2, SCREEN_HEIGHT-4-curfont->baseline, tm);
                    ConvertScreenBuf();
                    ShowScreen();
                }
            }else if(Tms - lastTshow > 99 && Timer < 5){ // change time value
                lastTshow = Tms;
                evtDisp = 0;
                FillScreen(0);
                choose_font(FONTN16);
                if(startflags && timetostartO == 0){ // countdown
                    char s[3] = {0,0,0}, *buf=s;
                    uint8_t N = 59 - current_time.S; // show "0" @ 59 seconds!
                    if(N == 0){ // countdown ends
                        sendstring("START!!!\n");
                        if(startflags & ST_FLAG_AUTO){ // rewind for autostart
                            timetostartO = timetostartA;
                        }else{ // clear autostart
                            timetostartA = 0;
                            startflags = 0;
                        }
                        choose_font(FONT14);
                        const char *strt = "�����!";
                        uint16_t w = GetStrWidth(strt);
                        PutStringAt((SCREEN_WIDTH-w-1)/2, SCREEN_HEIGHT-1-curfont->baseline, strt);
                    }else{
                        if(N >= 10){
                            *buf++ = N/10 + '0';
                            N %= 10;
                        }
                        *buf++ = (char)(N + '0');
                        uint16_t w = GetStrWidth(s);
                        PutStringAt((SCREEN_WIDTH-w-1)/2, SCREEN_HEIGHT-1-curfont->baseline, s);
                    }
                }else{
                    char *tm = get_scrntime(&current_time, get_millis());
                    tm[8] = 0; // throw out fractional parts
                    PutStringAt(5, SCREEN_HEIGHT-1-curfont->baseline, tm);
                    if(startflags){
                        if(current_time.S == 28){ // decrease timetostartO, prepare to countdown
                            --timetostartO;
                        }
                        char ltr = 'O';
                        if(startflags & ST_FLAG_AUTO) ltr = 'A';
                        choose_font(FONTN8);
                        DrawCharAt(SCREEN_WIDTH-8, SCREEN_HEIGHT-9, ltr);
                        DrawCharAt(SCREEN_WIDTH-8, SCREEN_HEIGHT-1, timetostartO + '0');
                    }
                }
                ConvertScreenBuf();
                ShowScreen();
            }
        }
        process_screen();
        IWDG->KR = IWDG_REFRESH;
        usb_proc();
        int r = 0;
        char *txt = NULL;
        if((txt = get_USB())){
            IWDG->KR = IWDG_REFRESH;
            parse_CMD(txt);
        }
        if(usartrx(1)){ // usart1 received data, store it in buffer
            r = usart_getline(1, &txt);
            IWDG->KR = IWDG_REFRESH;
            if(r){
                txt[r] = 0;
                if(the_conf.defflags & FLAG_GPSPROXY){
                    usart_send(GPS_USART, txt);
                }else{ // UART1 is additive serial/bluetooth console
                    usart_send(1, txt);
                    if(*txt != '\n'){
                        parse_CMD(txt);
                    }
                }
            }
        }
        if(usartrx(GPS_USART)){
            IWDG->KR = IWDG_REFRESH;
            r = usart_getline(GPS_USART, &txt);
            if(r){
                txt[r] = 0;
                if(the_conf.defflags & FLAG_GPSPROXY) usart_send(1, txt);
                GPS_parse_answer(txt);
            }
        }
        if(usartrx(LIDAR_USART)){
            IWDG->KR = IWDG_REFRESH;
            r = usart_getline(LIDAR_USART, &txt);
            if(r){
                if(the_conf.defflags & FLAG_NOLIDAR){
                    usart_send(LIDAR_USART, txt);
                    if(*txt != '\n'){
                        parse_CMD(txt);
                    }
                }else
                    parse_lidar_data(txt);
            }
        }
        chk_buzzer(); // should we turn off buzzer?
        chkTrig1(); // check trigger without interrupt
    }
    return 0;
}
