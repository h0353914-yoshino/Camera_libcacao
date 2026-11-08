package vendor.somc.cacao;

import android.view.Surface;
import vendor.somc.cacao.CacaoBlob;
import vendor.somc.cacao.CacaoImage;
import vendor.somc.cacao.ICacaoProcessCallback;

// 對應原 ICacao 的 8 個 transaction。呼叫端提供共享記憶體，server 直接寫入（與原版相同）。
// process 回傳改 long（修掉 32/64 位元跨 ABI 差異）。
@VintfStability
interface ICacao {
    void disconnect();
    int start(in CacaoBlob mode);
    int stop();
    int setConfig(int configId, in CacaoBlob data);
    int getConfig(int configId, in CacaoBlob data);
    long process(in CacaoBlob param, in CacaoImage[] images, in CacaoBlob result,
            int flags, @nullable ICacaoProcessCallback callback);
    long processToSurface(in CacaoBlob param, in Surface output, in CacaoBlob result,
            int flags, @nullable ICacaoProcessCallback callback);
    int cancel(int cookie);
}
