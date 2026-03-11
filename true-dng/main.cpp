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
static PaperProfile pfe_2383("./data/paper/kodak_2383/");
static PaperProfile pfe_2393("./data/paper/kodak_2393/");

//static PaperProfile ektacolor("./data/paper/kodak_ektacolor_edge/");
//static PaperProfile ultra("./data/paper/kodak_ultra_endura/");
//static PaperProfile portra("./data/paper/kodak_portra_endura/");

static FilmProfile film("./data/film/negative/generic_a/");

uint32_t paper_model = 1;
static bool monochrome = false;

glm::mat3 film_to_paper;

float dc = 1.0f;
float c = 1.0f, m = 1.0f, y = 1.0;
float contrast = dc, exposure = 1.0f;
uint32_t vp_xres = 1, vp_yres = 1;
glm::vec3 negative[64 * 1024 * 1024];

glm::vec3 tonemap(glm::vec3 neg_color)
{
    glm::vec3 pos_color = rec2020_to_rec709 * neg_color;
    if(paper_model == 1) pos_color = vp_tonemap(neg_color, contrast);
    else if(paper_model == 2) pos_color = paper_tonemap(endura, neg_color, contrast);
    else if(paper_model == 3) pos_color = paper_tonemap(pfe_2383, neg_color, contrast);
    if(monochrome) pos_color = glm::vec3(pos_color.g);
    return glm::pow(clamp(pos_color, 0.0f, 1.0f), glm::vec3(1.0f / 2.2f));;
}

inline uint32_t encode(const glm::vec3& in)
{
    uint32_t o = 0xff000000;
    for(uint32_t i = 0; i < 3; ++i)
        ((uint8_t*)&o)[2 - i] = (uint8_t)(in[i] * 255.0f + 0.5f);
    return o;
}

static uint32_t frame_buffer[XRES * YRES];
void render_preview(tinydng::DNGImage& src)
{
    for(uint32_t i = 0; i < XRES * YRES; ++i)
    {
        glm::ivec2 fbc(i % XRES, i / XRES);
        glm::vec2 uv = glm::vec2(fbc) / glm::vec2(XRES, YRES);
        uv.y = 1.0f - uv.y;

        glm::ivec2 vpc = uv * glm::vec2(src.width, src.height);
        uint32_t ni =  vpc.y * src.width + vpc.x ;

        glm::vec3 neg_color = negative[ni] * glm::vec3(c, m, y) * exposure;
        glm::vec3 pos_color = tonemap(neg_color);
        frame_buffer[i] = encode(pos_color);
    }
}

static void save_image(tinydng::DNGImage& src, std::string filename)
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

                //glm::vec3 raw_out = invert(neg_color, gamma - 1.5f);
                //raw_out = rec2020_to_xyz * raw_out;
                //raw_out = glm::clamp(raw_out * scale_factor, 0.0f, 1.0f);
                //raw_data[i * 3 + 0] = (uint16_t)(raw_out.r * 0xffff + 0.5f);
                //raw_data[i * 3 + 1] = (uint16_t)(raw_out.g * 0xffff + 0.5f);
                //raw_data[i * 3 + 2] = (uint16_t)(raw_out.b * 0xffff + 0.5f);

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
        tbb::parallel_for(tbb::blocked_range<int>(0, src.width * src.height), [&](tbb::blocked_range<int> r)
        {
            for(uint32_t i = r.begin(); i < r.end(); ++i)
            {
                glm::vec3 neg_color = negative[i] * exposure;

                //float raw_out = 0.01f / neg_color;
                //raw_out = glm::clamp(raw_out * scale_factor, 0.0f, 1.0f);
                //raw_data[i] = (uint16_t)(raw_out * 0xffff + 0.5f);

                float png_out = tonemap(neg_color).g;
                png_data[i] = (uint8_t)(png_out * 0xff + 0.5f);
            }
        });
        dng_image.SetImageData((uint8_t*)raw_data.data(), raw_data.size() * 2);
    }

    //write file
    std::string err;
    tinydngwriter::DNGWriter dng_writer(big_endian);
    dng_writer.AddImage(&dng_image);
    //dng_writer.WriteToFile(("output/dngs/" + filename + ".dng").c_str(), &err);

    stbi_write_jpg(("output/" + filename + ".jpg").c_str(), src.width, src.height, src.samples_per_pixel, png_data.data(), 100);
}

int main(int argc, char* argv[])
{

    //film_to_paper = coupling_matrix(film, paper);
    glm::vec3 grey = vp_tonemap(glm::vec3(0.18f));
    printf("%f, %f, %f\n", grey.x, grey.y, grey.z);

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
        std::string filename = "test";
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

        if(images[0].samples_per_pixel == 3)
        {
            glm::mat3 cc;
            for(uint32_t i = 0; i < 3; ++i)
                for(uint32_t j = 0; j < 3; ++j)
                {
                    cc[j][i] = images[1].color_matrix1[i][j];
                }

            glm::mat3 forward_matrix = glm::inverse(cc);
            glm::mat3 inv = glm::inverse(forward_matrix);
            glm::vec3 scale = inv * d65_xyz;
            forward_matrix[0] *= scale.x;
            forward_matrix[1] *= scale.y;
            forward_matrix[2] *= scale.z;

            for(uint32_t i = 0; i < images[0].width * images[0].height; ++i)
            {
                uint16_t* data = ((uint16_t*)images[0].data.data()) + i * 3;
                negative[i] = glm::vec3(data[0], data[1], data[2]) / glm::vec3(65535.0f) * 2.0f;
                negative[i] = xyz_to_rec2020 * forward_matrix * negative[i];
            }
        }
        else
        {
            monochrome = true;
            contrast = dc = 0.7f;
            for(uint32_t i = 0; i < images[0].width * images[0].height; ++i)
            {
                uint16_t* data = ((uint16_t*)images[0].data.data()) + i;
                negative[i] = glm::vec3(data[0]) / glm::vec3(65535.0f) * 2.0f;
            }
        }

        do
        {
            static MSG dummy_message;
            PeekMessageA(&dummy_message, 0, 0, 0, 1);

            render_preview(images[0]);

            SetDIBitsToDevice(hdc, 0, 0, XRES, YRES, 0, 0, 0, YRES, frame_buffer, &bmi, DIB_RGB_COLORS);
            Sleep(16);

            if(GetAsyncKeyState('X'))
            {
                glm::vec3 sum(0.0f);
                for(uint32_t i = 0; i < images[0].width * images[0].height; ++i)
                    sum += 1.0f / negative[i];

                sum /= sum.g;
                c = sum.r;
                m = sum.g;
                y = sum.b;
                contrast = dc;
                exposure = 1.0f;
            }

            float inc = 1.01;
            if(GetAsyncKeyState('1')) paper_model = 1;
            if(GetAsyncKeyState('2')) paper_model = 2;
            if(GetAsyncKeyState('3')) paper_model = 3;
            if(GetAsyncKeyState('4')) paper_model = 4;
            if(GetAsyncKeyState('0')) paper_model = 7;

            if(GetAsyncKeyState(VK_SHIFT)) inc = 1.05;

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

            if(GetAsyncKeyState(VK_ESCAPE)) return 0;
            if(GetAsyncKeyState(VK_RETURN))
            {
                frame++;
                save_image(images[0], filename);
                break;
            }
        } 
        while(true);
    } 
    while(true);


    return 0;
}