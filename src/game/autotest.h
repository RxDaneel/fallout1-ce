#ifndef FALLOUT_AUTOTEST_H_
#define FALLOUT_AUTOTEST_H_

#ifdef FALLOUT_AUTOTEST

namespace fallout {

void autotestTick();
void autotestGetMouseInput(int* dx, int* dy, int* buttons);

} // namespace fallout

#endif

#endif /* FALLOUT_AUTOTEST_H_ */
