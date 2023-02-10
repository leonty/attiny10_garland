#include <avr/io.h>
#include <avr/power.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>

// Паттерны мигания. Начальное состояние - выключено. Ноль - конец последовательности. 
// Если число положительное, то это переход во включенное состояние, иначе в выключенное.
// Если число меньше 128, то это мгновенный переход в состояние, иначе плавный через ШИМ в течение n миллисекунд.
// Если знак очередного числа равен предыдущему, то это просто задержка.
// Пример: 
// 2000, 1000, -1, -500, 1, 500, -1, -500, 1, 500, -1, -500, 1, 1000, -2000
// плавно в течение 2 секунд включили, подождали секунду, мигнули 3 раза (по 500 мс), подождали секунду, плавно в течение 2 секунд выключили

const int * const patterns[] = {
    (const int16_t[]){500, -500, 500, -500, 1, 50, -1, -50, 1, 50, -1, -50, 1, 50, -3000, -2000, 0},
    (const int16_t[]){1, 500, -1, -500, 0},
    (const int16_t[]){4000, -1, -2000, 0},
    (const int16_t[]){1, 250, -1, -250, 0},
    (const int16_t[]){500, -500, -3000, 0},
    (const int16_t[]){1, 50, -1, -50, 0},
    (const int16_t[]){1, -4000, -2000, 0},
    (const int16_t[]){2000, -1, -100, 1, 100, -1, -100, 1, 100, -1, -100, 1, 100, -1, -100, 1, -2000, -3000, 0},
    (const int16_t[]){1000, -1000, 0}
};

// Каждый паттерн проигрывается 30 сек
const uint16_t PATTERN_DURATION = 30000;

// Период ШИМ должен увеличиваться/уменьшаться нелинейно чтобы компенсировать меньшую чувствительность глаза к более ярким состояниям диодов.
// В массиве находятся точки ускорения/замедления, после которых шаг приращения периода увеличивается/уменьшается на 1.
// Первый и последний элементы - ограничительные.
const uint8_t accelerationPoints[] = {0, 30, 60, 80, 10, 255};

// Счётчик системных тиков
volatile uint16_t ticks = 0;

ISR(TIM0_OVF_vect)
{
    ++ticks;
}

// Задержка с использованием sleep mode. Каждое переполнение таймера будит цикл.
void sleep(const uint16_t & delay)
{
    uint16_t end = ticks + delay;

    do
    {
        set_sleep_mode(SLEEP_MODE_IDLE);  
        sleep_enable();
        sleep_cpu(); 
        sleep_disable();
    } 
    while (end > ticks);
}

static void setPwmDutyCycle(const uint16_t & value)
{
    cli();
    OCR0A = value;
    sei();
}

static void setup()
{
    // Уменьшение энергопотребления для режима Idle
    // Отключение ADC
    power_adc_disable();

    // Отключение analog comparator
    ACSR ^= ~_BV(ACIE);
    ACSR |= ACD;

    // Установка тактирования на Internal 128 kHz Oscillator
    CCP = 0xD8;
    CLKMSR = 0b01;
    CCP = 0xD8;
    CLKPSR = 0;

    // Настройка ШИМ на порту PB0
    DDRB |= _BV(DDB0);

    cli();

    TCCR0A = 0;
    TCCR0B = 0;

    // Режим - FastPWM 8 bit, вывод на OC0A, частота - system clock, TOP=128
    TCCR0A = _BV(COM0A1) | _BV(WGM01);
    TCCR0B = _BV(CS00) | _BV(WGM02) | _BV(WGM03);
    ICR0 = 128;
    OCR0A = 0;

    sei();

    // Переполнение таймера также отсчитывает системные тики (1мс)
    TIMSK0 |= _BV(TOIE0);
}

int main()
{
    setup();

    // Текущее состояние, true = включено, false = выключено
    bool state = false;

    while (true)
    {   
        // Перебираем все паттерны по очереди
        for (uint8_t i = 0; i < sizeof(patterns)/sizeof(patterns[0]); i++)
        {
            // Сбрасываем счётчик системных тиков, которого хватает на 65 сек (1кГц*16бит) чтобы не попасть на переполнение
            ticks = 0; 

            // Каждый паттерн крутим в течение PATTERN_DURATION
            do 
            {
                // Перебираем все ноты
                for (uint8_t j = 0; patterns[i][j] != 0; j++)
                {
                    int16_t delay = patterns[i][j];
                    bool sign = true;

                    // Сохраняем знак и оставляем модуль задержки
                    if (delay < 0)
                    {
                        delay = -delay;
                        sign = false;
                    } 

                    // Если текущее состояние не изменяется, то просто спим
                    if (state == sign)
                    {   
                        sleep(delay);
                    }
                    // Иначе переключаем состояние
                    else
                    {
                        // Быстро
                        if (delay < 128)
                        {
                            setPwmDutyCycle(sign ? 127 : 0);
                        }
                        // Или плавно
                        else
                        {
                            // Используется 64 шага для плавного включения/выключения с помощью ШИМ. 
                            // Так как задержки не делятся нацело на 64, сохраняем остаток от деления для поправки задержек
                            uint8_t remainder = delay % 64;

                            // Шаг приращения периода ШИМ меняется из-за нелинейной зависимости яркости от заполения
                            uint8_t step;

                            if (sign)
                            {
                                // Если включаемся, то начинаем делать это медленно
                                step = 1;
                            }
                            else
                            {
                                // Если выключаемся, то начинаем делать это быстро
                                step = sizeof(accelerationPoints) / sizeof(accelerationPoints[0]) - 2;
                            }

                            // Проходим 128 ступеней ШИМ с задержкой delay/64 на каждой итерации и прыгая через step ступеней
                            for (uint8_t k = 1; k < 128; k += step)
                            {
                                uint8_t value;

                                if (sign)
                                {
                                    value = k - 1;

                                    // Реагируем на точки ускорения
                                    if (value >= accelerationPoints[step])
                                    {
                                        ++step;
                                    }
                                }
                                else
                                {
                                    value = 128 - k;

                                    // Реагируем на точки замедления
                                    if (value <= accelerationPoints[step - 1])
                                    {
                                        --step;
                                    }
                                }

                                // Устанавливаем период ШИМ для данной итерации
                                setPwmDutyCycle(value);

                                // Учитываем "потерянное" на целочисленном делении время сна
                                if (remainder)
                                {
                                    --remainder;
                                    sleep((delay / 64) + 1);                                
                                }
                                else
                                {
                                    sleep(delay / 64);                                
                                }
                            }
                        }

                        state = sign;
                    }
                }
            } while (ticks < PATTERN_DURATION);
        }
    }
}