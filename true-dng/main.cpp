#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <Windows.h>
#include <tbb/parallel_for.h>

#undef max
#undef min
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINY_DNG_LOADER_IMPLEMENTATION
#define TINY_DNG_WRITER_IMPLEMENTATION
#include "stb_image_write.h"
#include "tiny_dng_loader.h"
#include "tiny_dng_writer.h"

#include <algorithm>
#include <glm/glm.hpp>
#include "color.hpp"

constexpr uint32_t XRES = 256 * 3;
constexpr uint32_t YRES = 256 * 2;

const static PIXELFORMATDESCRIPTOR pfd = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const static BITMAPINFO bmi = {{sizeof(BITMAPINFOHEADER),  XRES, YRES,  1, 32, BI_RGB, XRES * YRES * 4}, {0}};
static DEVMODE screenSettings = {{ 0 }, 0, 0, 156, 0, 0x001c0000, { 0 }, 0, 0, 0, 0, 0, { 0 }, 0, 32, XRES, YRES, { 0 }, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static bool big_endian = false;

static PaperProfile endura("./data/paper/kodak_endura_premier/");
static PaperProfile portra("./data/paper/kodak_portra_endura/");
static PaperProfile ultra("./data/paper/kodak_ultra_endura/");
static PaperProfile ektacolor("./data/paper/kodak_ektacolor_edge/");

static PaperProfile pfe_2383("./data/paper/kodak_2383/");
static PaperProfile pfe_2393("./data/paper/kodak_2393/");

static FilmProfile film("./data/film/negative/generic_a/");

uint32_t paper_model = 0.0;

glm::mat3 film_to_paper;

glm::mat3 forward_matrix(
    glm::vec3(0.21921, 0.08772, 0.00000),
    glm::vec3(0.13847, 0.57034, 0.00000),
    glm::vec3(0.19788, 0.01196, 0.84502));

float c = 1.0f, m = 1.0f, y = 1.0;
float contrast = 2.0, exposure = 1.0f, shadow_density = 1.0f;
float black_point(0.0f);
float white_point(0.0f);
uint32_t vp_xres = 1, vp_yres = 1;
glm::vec3 negative[64 * 1024 * 1024];

glm::vec3 cyan(1.0, 0.0f, 0.0f);
glm::vec3 magenta(0.0, 1.0f, 0.0f);
glm::vec3 yellow(0.0, 0.0f, 1.0f);

uint32_t histogram[1024];

inline void write_histo(glm::vec3& color)
{
    float value = (color.x + color.y + color.z) / 3.0f;
    float log_exp = glm::log2(value / 0.18f);
    uint32_t index = log_exp * 8 + 512;
    if(index > 0) index = 512;
    histogram[index]++;
}

static glm::vec3 sigmoid(glm::vec3 in, float contrast)
{
    //glm::vec3 out = glm::log(in) - glm::log(glm::vec3(0.18));
    //out = glm::exp(out * contrast + glm::log(glm::vec3(0.18)));

    glm::vec3 out = glm::pow(in, glm::vec3(contrast)) * glm::pow(0.18f, 1.0f - contrast);
    return out / (out + 1.0f);
}

static glm::vec3 paper_tonemap(const PaperProfile& paper, const glm::vec3& in)
{
    glm::vec3 out = paper.xyz_to_sens * rec2020_to_xyz * in;
    out.x = 1.0 / sample(paper.r_curve, out.x);
    out.y = 1.0 / sample(paper.g_curve, out.y);
    out.z = 1.0 / sample(paper.b_curve, out.z);
    out = xyz_to_rec709 * paper.dye_to_xyz * out;
    out = glm::pow(out, glm::vec3(1.0 / 2.2f));
    return clamp(out, 0.0f, 1.0f);
}

glm::vec3 tonemap(glm::vec3 neg_color)
{
         if(paper_model == 1) return paper_tonemap(endura, neg_color);
    else if(paper_model == 2) return paper_tonemap(portra, neg_color);
    else if(paper_model == 3) return paper_tonemap(ultra, neg_color);
    else if(paper_model == 4) return paper_tonemap(ektacolor, neg_color);
    else if(paper_model == 5) return paper_tonemap(pfe_2383, neg_color);

    //variable contrast reinhard tonemap in rec2020
    glm::vec3 pos_color = 0.01f / neg_color - shadow_density * 0.01f / white_point;
    pos_color = sigmoid(pos_color, contrast);
    pos_color = rec2020_to_rec709 * pos_color;
    pos_color = clamp(pos_color, 0.0f, 1.0f);
    pos_color = glm::pow(pos_color, glm::vec3(1.0f / 2.2f));
    return pos_color;
}

inline uint32_t encode(const glm::vec3& in)
{
    uint32_t o = 0xff000000;
    for(uint32_t i = 0; i < 3; ++i)
        ((uint8_t*)&o)[2 - i] = (uint8_t)(in[i] * 255.0f + 0.5f);
    return o;
}

static uint32_t frame_buffer[XRES * YRES];
void render_paper(tinydng::DNGImage& src)
{
    glm::vec3 wp(0.0f);
    glm::vec3 bp(1.0e6f);
    for(uint32_t i = 0; i < XRES * YRES; ++i)
    {
        glm::ivec2 fbc(i % XRES, i / XRES);
        glm::vec2 uv = glm::vec2(fbc) / glm::vec2(XRES, YRES);
        uv.y = 1.0f - uv.y;

        glm::ivec2 vpc = uv * glm::vec2(src.width, src.height);
        uint32_t ni =  vpc.y * src.width + vpc.x ;

        glm::vec3 neg_color = negative[ni] * glm::vec3(c, m, y) * exposure;

        wp = glm::max(wp, neg_color);
        bp = glm::min(bp, neg_color);

        glm::vec3 pos_color = tonemap(neg_color);
        pos_color = glm::clamp(pos_color, 0.0f, 1.0f);
        frame_buffer[i] = encode(pos_color);
    }

    black_point = glm::min(bp.r, glm::min(bp.g, bp.b));
    white_point = glm::max(wp.r, glm::max(wp.g, wp.b));
}

std::string filename = "02-25-2026-xx-";
static void write_image(tinydng::DNGImage& src)
{
    tinydngwriter::DNGImage dng_image;
	dng_image.SetBigEndian(big_endian);

    uint8_t dng_version[4] = {1, 4, 0, 0};
    dng_image.SetTagByte(tinydngwriter::TIFFTAG_DNG_VERSION, dng_version, 4);
    dng_image.SetImageDescription("True DNG");

	dng_image.SetSubfileType(false, false, false);
	dng_image.SetImageWidth(src.width);
	dng_image.SetImageLength(src.height);
	dng_image.SetRowsPerStrip(src.height);

	// SetSamplesPerPixel must be called before SetBitsPerSample()
	uint16_t bps[3] = {16, 16, 16};
	dng_image.SetSamplesPerPixel(src.samples_per_pixel);
	dng_image.SetBitsPerSample(src.samples_per_pixel, bps);

	dng_image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
	dng_image.SetCompression(tinydngwriter::COMPRESSION_NONE);
	dng_image.SetPhotometric(tinydngwriter::PHOTOMETRIC_LINEARRAW);

    dng_image.SetOrientation(1);
    dng_image.SetXResolution(1.0);
    dng_image.SetYResolution(1.0);
    dng_image.SetResolutionUnit(tinydngwriter::RESUNIT_NONE);

    double baseline_exposure = 1.0f;
    float scale_factor = 0.5f * powf(2.0f, -baseline_exposure);
    dng_image.SetTagSrational(tinydngwriter::TIFFTAG_BASELINE_EXPOSURE, &baseline_exposure, 1);

    std::vector<uint8_t> png_data(src.width * src.height * src.samples_per_pixel);
    std::vector<uint16_t> raw_data(src.width * src.height * src.samples_per_pixel);
    if(src.samples_per_pixel == 3)
    {
        //camera calibration matrices
        dng_image.SetCalibrationIlluminant1(tinydng::LIGHTSOURCE_D65);

        double identity[9] = {
            1.0, 0.0, 0.0,
            0.0, 1.0, 0.0,
            0.0, 0.0, 1.0
        };

        dng_image.SetColorMatrix1(3, identity);
        dng_image.SetCameraCalibration1(3, identity);

        double analog_balance[3] = {1.0, 1.0, 1.0};
        dng_image.SetTagRational(tinydngwriter::TIFFTAG_ANALOG_BALANCE, analog_balance, 3);

        double as_shot_neutral[3] = {d65_xyz.x, d65_xyz.y, d65_xyz.z}; //D65 in CIE XYZ
        //double as_shot_neutral[3] = {1.0, 1.0, 1.0}; //D65 in rec709
        dng_image.SetTagRational(tinydngwriter::TIFFTAG_AS_SHOT_NEUTRAL, as_shot_neutral, 3);

        tbb::parallel_for(tbb::blocked_range<int>(0, src.width * src.height), [&](tbb::blocked_range<int> r)
        {
            for(uint32_t i = r.begin(); i < r.end(); ++i)
            {
                glm::vec3 neg_color = negative[i] * glm::vec3(c, m, y) * exposure;

                glm::vec3 raw_out = 0.01f / neg_color - 0.01f / white_point;
                raw_out = glm::pow(raw_out, glm::vec3(1.5f)) * glm::pow(0.18f, -0.5f);
                raw_out = rec2020_to_xyz * raw_out;
                raw_out = glm::clamp(raw_out * scale_factor, 0.0f, 1.0f);
                raw_data[i * 3 + 0] = (uint16_t)(raw_out.r * 0xffff + 0.5f);
                raw_data[i * 3 + 1] = (uint16_t)(raw_out.g * 0xffff + 0.5f);
                raw_data[i * 3 + 2] = (uint16_t)(raw_out.b * 0xffff + 0.5f);

                glm::vec3 png_out = tonemap(neg_color);
                png_data[i * 3 + 0] = (uint8_t)(png_out.r * 0xff + 0.5f);
                png_data[i * 3 + 1] = (uint8_t)(png_out.g * 0xff + 0.5f);
                png_data[i * 3 + 2] = (uint8_t)(png_out.b * 0xff + 0.5f);
            }
        });
        dng_image.SetImageData((uint8_t*)raw_data.data(), raw_data.size() * 2);
    }
    else
    {
        for(uint32_t i = 0; i < raw_data.size(); ++i)
        {
            float raw_out = 0.01f / negative[i].r;
            raw_out = glm::clamp(raw_out * scale_factor, 0.0f, 1.0f);
            raw_data[i] = (uint16_t)(raw_out * 0xffff + 0.5f);

            float png_out = tonemap(negative[i]).r;
            png_data[i] = (uint8_t)(png_out * 0xff + 0.5f);
        }
        dng_image.SetImageData((uint8_t*)raw_data.data(), raw_data.size() * 2);
    }

    //write file
    std::string err;
    tinydngwriter::DNGWriter dng_writer(big_endian);
    dng_writer.AddImage(&dng_image);
    dng_writer.WriteToFile(("output/" + filename + ".dng").c_str(), &err);

    stbi_write_jpg(("output/" + filename + ".jpg").c_str(), src.width, src.height, src.samples_per_pixel, png_data.data(), 100);
}

int main(int argc, char* argv[])
{

    //film_to_paper = coupling_matrix(film, paper);
    cyan = rec709_to_xyz * cyan;
    yellow = rec709_to_xyz * yellow;
    magenta = rec709_to_xyz * magenta;

    RECT rect;
    GetClientRect(GetDesktopWindow(), &rect);
    rect.left = (rect.right / 2) - (XRES / 2);
    rect.top = (rect.bottom / 2) - (YRES / 2);

    HWND hwnd = CreateWindowExA(WS_EX_APPWINDOW, "static", "Digital Enlarger", WS_VISIBLE | WS_POPUP, rect.left, rect.top, XRES, YRES, 0, 0, 0, 0);

    HDC hdc = GetDC(hwnd);
    SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);

    uint32_t frame = 0;

    do
    {
        if(argc > 1) filename = std::string(argv[1]);
        filename += std::to_string(frame);

        std::string warn, err;
        std::vector<tinydng::DNGImage> images;
        std::vector<tinydng::FieldInfo> custom_field_lists;
        bool ret = tinydng::LoadDNG(("input/" + filename + ".dng").c_str(), custom_field_lists, &images, &warn, &err);
        if(!ret)
        {
            printf("%s\n", err.c_str());
            return 0;
        }

        glm::mat3 cc;
        for(uint32_t i = 0; i < 3; ++i)
            for(uint32_t j = 0; j < 3; ++j)
            {
                cc[j][i] = images[1].color_matrix1[i][j];
            }

        forward_matrix = glm::inverse(cc);
        glm::mat3 inv = glm::inverse(forward_matrix);
        glm::vec3 scale = inv * d65_xyz;
        forward_matrix[0] *= scale.x;
        forward_matrix[1] *= scale.y;
        forward_matrix[2] *= scale.z;

        if(images[0].samples_per_pixel == 3)
        {
            for(uint32_t i = 0; i < images[0].width * images[0].height; ++i)
            {
                uint16_t* data = ((uint16_t*)images[0].data.data()) + i * 3;
                negative[i] = glm::vec3(data[0], data[1], data[2]) / glm::vec3(65535.0f);
                negative[i] = xyz_to_rec2020 * forward_matrix * negative[i];
            }
        }
        else
        {
            for(uint32_t i = 0; i < images[0].width * images[0].height; ++i)
            {
                uint16_t* data = ((uint16_t*)images[0].data.data()) + i;
                negative[i] = glm::vec3(data[0]) / glm::vec3(65535.0f);
            }
        }

        do
        {
            static MSG dummy_message;
            PeekMessageA(&dummy_message, 0, 0, 0, 1);

            render_paper(images[0]);

            SetDIBitsToDevice(hdc, 0, 0, XRES, YRES, 0, 0, 0, YRES, frame_buffer, &bmi, DIB_RGB_COLORS);
            Sleep(16);

            float inc = 1.01;
            if(GetAsyncKeyState('0')) paper_model = 0;
            if(GetAsyncKeyState('1')) paper_model = 1;
            if(GetAsyncKeyState('2')) paper_model = 2;
            if(GetAsyncKeyState('3')) paper_model = 3;
            if(GetAsyncKeyState('4')) paper_model = 4;
            if(GetAsyncKeyState('5')) paper_model = 5;

            if(GetAsyncKeyState(VK_SHIFT)) inc = 1.10;

            if(GetAsyncKeyState('Q')) c *= inc;
            if(GetAsyncKeyState('A')) c /= inc;
            if(GetAsyncKeyState('W')) m *= inc;
            if(GetAsyncKeyState('S')) m /= inc;
            if(GetAsyncKeyState('E')) y *= inc;
            if(GetAsyncKeyState('D')) y /= inc;

            if(GetAsyncKeyState(VK_UP))   exposure /= inc;
            if(GetAsyncKeyState(VK_DOWN)) exposure *= inc;

            if(GetAsyncKeyState(VK_LEFT)) contrast /= inc;
            if(GetAsyncKeyState(VK_RIGHT)) contrast *= inc;

            if(GetAsyncKeyState('Z')) black_point /= inc;
            if(GetAsyncKeyState('X')) black_point *= inc;

            if(GetAsyncKeyState(VK_ESCAPE)) return 0;
            if(GetAsyncKeyState(VK_RETURN))
            {
                frame++;
                write_image(images[0]);
                break;
            }
        } 
        while(true);
    } 
    while(true);

    return 0;
}