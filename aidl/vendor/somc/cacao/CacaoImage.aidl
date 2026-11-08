package vendor.somc.cacao;

import android.hardware.common.NativeHandle;

// 取代 ImageBuf 的 {w,h,dataSpace,native_handle}。
@VintfStability
parcelable CacaoImage {
    int width;
    int height;
    int dataSpace;
    NativeHandle handle;
}
