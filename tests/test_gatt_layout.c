/*
 * Pins the Ring Service's attribute layout at compile time.
 *
 * ble.c reaches into the service table by raw index -- ATTR_STATUS is
 * `&ring_svc.attrs[1]`, ATTR_ACTIVITY is `&ring_svc.attrs[12]` -- because
 * that is what bt_gatt_notify() wants: it accepts a Characteristic
 * Declaration and resolves the value handle itself. The indices are only
 * correct for one particular ordering of the BT_GATT_* macros, and nothing
 * in C makes them follow if somebody inserts a characteristic in the
 * middle. A wrong index does not fail to build and does not crash; it
 * notifies the wrong attribute, on a service that (per STATUS.md R10) no
 * phone has ever exercised.
 *
 * So: include ble.c against the stub GATT macros, which expand to the same
 * number of array entries as the real ones (two per characteristic, one
 * per CCC), and assert the total. Add or reorder a characteristic and this
 * stops the build, which is the moment to revisit the ATTR_* constants and
 * the layout comment beside them.
 *
 * This is a compile-time check only. It cannot tell you that index 12 is
 * the *activity* declaration rather than some other attribute -- only that
 * the table is still the size the indices were written against.
 */

#include <assert.h>

#include "../src/ble.c"

/*
 *   0  primary service
 *   1  status   decl   2 value   3 CCC
 *   4  ppg      decl   5 value   6 CCC
 *   7  imu      decl   8 value   9 CCC
 *  10  control  decl  11 value        (write-only, no CCC)
 *  12  activity decl  13 value  14 CCC
 */
#define EXPECTED_ATTRS	15

_Static_assert(ARRAY_SIZE(ring_svc_attrs) == EXPECTED_ATTRS,
	       "Ring Service attribute count changed -- the ATTR_* indices in "
	       "ble.c and the layout comment beside them need rechecking");

/* Each named index must be inside the table. */
_Static_assert(12 < EXPECTED_ATTRS, "ATTR_ACTIVITY out of range");

/*
 * No main(): this translation unit exists for its assertions and is only
 * ever compiled, never linked -- linking it would mean supplying the whole
 * Bluetooth host. `make syntax` builds it with -fsyntax-only.
 */
