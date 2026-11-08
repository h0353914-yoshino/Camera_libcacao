package vendor.somc.cacao;

import vendor.somc.cacao.CacaoBlob;
import vendor.somc.cacao.ICacao;
import vendor.somc.cacao.ICacaoClient;

// 對應原 ICacaoService 的 4 個 transaction。
@VintfStability
interface ICacaoService {
    ICacao connect(ICacaoClient client);
    int getServicePid();
    int getCaps(int cameraIndex, in CacaoBlob data);
    int getJpegBufferSize(int width, int height);
}
