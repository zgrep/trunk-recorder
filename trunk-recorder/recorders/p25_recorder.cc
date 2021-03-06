
#include "p25_recorder.h"
#include <boost/log/trivial.hpp>
#include "../formatter.h"
#include "../../lib/gr_blocks/nonstop_wavfile_delayopen_sink_impl.h"


p25_recorder_sptr make_p25_recorder(Source * src)
{
  p25_recorder * recorder = new p25_recorder();
  //recorder->initialize(src, gr::blocks::nonstop_wavfile_sink_impl::make(1, 8000, 16, false));
  recorder->initialize(src, gr::blocks::nonstop_wavfile_sink_impl::make(1, 8000, 16, true));
  return gnuradio::get_initial_sptr(recorder);
}

void p25_recorder::generate_arb_taps() {

  double arb_size  = 32;
  double arb_atten = 100;
// Create a filter that covers the full bandwidth of the output signal

// If rate >= 1, we need to prevent images in the output,
// so we have to filter it to less than half the channel
// width of 0.5.  If rate < 1, we need to filter to less
// than half the output signal's bw to avoid aliasing, so
// the half-band here is 0.5*rate.
double percent = 0.80;

if (arb_rate <= 1) {
  double halfband = 0.5 * arb_rate;
  double bw       = percent * halfband;
  double tb       = (percent / 2.0) * halfband;

  // BOOST_LOG_TRIVIAL(info) << "Arb Rate: " << arb_rate << " Half band: " << halfband << " bw: " << bw << " tb: " <<
  // tb;

  // As we drop the bw factor, the optfir filter has a harder time converging;
  // using the firdes method here for better results.
  arb_taps = gr::filter::firdes::low_pass_2(arb_size, arb_size, bw, tb, arb_atten, gr::filter::firdes::WIN_BLACKMAN_HARRIS);
} else {
  BOOST_LOG_TRIVIAL(error) << "Something is probably wrong! Resampling rate too low";
  exit(0);
}
}

p25_recorder::p25_recorder()
  : gr::hier_block2("p25_recorder",
                    gr::io_signature::make(1, 1, sizeof(gr_complex)),
                    gr::io_signature::make(0, 0, sizeof(float))), Recorder("P25")
{
}

p25_recorder::p25_recorder(std::string type)
  : gr::hier_block2("p25_recorder",
                    gr::io_signature::make(1, 1, sizeof(gr_complex)),
                    gr::io_signature::make(0, 0, sizeof(float))), Recorder(type)
{
}

p25_recorder::DecimSettings p25_recorder::get_decim(long speed) {
    long s = speed;
    long if_freqs[] = {24000, 25000, 32000};
    DecimSettings decim_settings={-1,-1};
    for (int i = 0; i<3; i++) {
        long if_freq = if_freqs[i];
        if (s % if_freq != 0) {
            continue;
        }
        long q = s / if_freq;
        if (q & 1) {
            continue;
        }
        
        if ((q >= 40) && ((q & 3) ==0)) {
            decim_settings.decim = q/4;
            decim_settings.decim2 = 4;
        } else {
            decim_settings.decim = q/2;
            decim_settings.decim2 = 2;
        }
        std::cout << "Decim: " << decim_settings.decim << " Decim2:  " << decim_settings.decim2 << std::endl;
        return decim_settings;
    }
    std::cout << "Nothing found" << std::endl;
    return decim_settings;
}
void p25_recorder::initialize_prefilter() {
  double phase1_channel_rate = phase1_symbol_rate * phase1_samples_per_symbol;
  //double phase2_channel_rate = phase2_symbol_rate * phase2_samples_per_symbol;
  long if_rate = phase1_channel_rate;
  long fa = 0;
  long fb = 0;
  if1 = 0;
  if2 = 0;
  samples_per_symbol  = phase1_samples_per_symbol; 
  symbol_rate         = phase1_symbol_rate;
  system_channel_rate = symbol_rate * samples_per_symbol;

  valve = gr::blocks::copy::make(sizeof(gr_complex));
  valve->set_enabled(false);
  lo = gr::analog::sig_source_c::make(input_rate, gr::analog::GR_SIN_WAVE, 0, 1.0, 0.0);
  mixer = gr::blocks::multiply_cc::make(); 

  p25_recorder::DecimSettings decim_settings = get_decim(input_rate);
  if (decim_settings.decim != -1) {
    double_decim = true;
    decim = decim_settings.decim;
    if1 = input_rate / decim_settings.decim;
    if2 = if1 / decim_settings.decim2;
    fa = 6250;
    fb = if2 / 2;
    BOOST_LOG_TRIVIAL(info) << "\t P25 Recorder two-stage decimator - Initial decimated rate: "<< if1 << " Second decimated rate: " << if2  << " FA: " << fa << " FB: " << fb << " System Rate: " << input_rate;
    bandpass_filter_coeffs = gr::filter::firdes::complex_band_pass(1.0, input_rate, -if1/2, if1/2, if1/2);
    lowpass_filter_coeffs = gr::filter::firdes::low_pass(1.0, if1, (fb+fa)/2, fb-fa);
    bandpass_filter = gr::filter::fft_filter_ccc::make(decim_settings.decim, bandpass_filter_coeffs);
    lowpass_filter = gr::filter::fft_filter_ccf::make(decim_settings.decim2, lowpass_filter_coeffs);
    resampled_rate = if2;
    bfo = gr::analog::sig_source_c::make(if1, gr::analog::GR_SIN_WAVE, 0, 1.0, 0.0);
    bandpass_filter->set_max_output_buffer(4096);
    bfo->set_max_output_buffer(4096);
  } else {
    double_decim = false;
    BOOST_LOG_TRIVIAL(info) << "\t P25 Recorder single-stage decimator - Initial decimated rate: "<< if1 << " Second decimated rate: " << if2  << " Initial Decimation: " << decim << " System Rate: " << input_rate;
    lo = gr::analog::sig_source_c::make(input_rate, gr::analog::GR_SIN_WAVE, 0, 1.0, 0.0);
    lowpass_filter_coeffs = gr::filter::firdes::low_pass(1.0, input_rate, 7250, 1450);
    decim = floor(input_rate / if_rate);
    resampled_rate = input_rate / decim;
    
    lowpass_filter = gr::filter::fft_filter_ccf::make(decim, lowpass_filter_coeffs);
    resampled_rate = input_rate / decim;
    lo->set_max_output_buffer(4096);
  }

  // Cut-Off Filter
  fa = 6250;
  fb = fa + 625;
  cutoff_filter_coeffs = gr::filter::firdes::low_pass(1.0, if_rate, (fb+fa)/2, fb-fa);
  cutoff_filter = gr::filter::fft_filter_ccf::make(1.0, cutoff_filter_coeffs);

  // ARB Resampler
  arb_rate = if_rate / resampled_rate;
  generate_arb_taps();
  arb_resampler = gr::filter::pfb_arb_resampler_ccf::make(arb_rate, arb_taps);
  BOOST_LOG_TRIVIAL(info) << "\t P25 Recorder ARB - Initial Rate: "<< input_rate << " Resampled Rate: " << resampled_rate  << " Initial Decimation: " << decim << " System Rate: " << system_channel_rate << " ARB Rate: " << arb_rate;
 
  // Squelch DB
  // on a trunked network where you know you will have good signal, a carrier
  // power squelch works well. real FM receviers use a noise squelch, where
  // the received audio is high-passed above the cutoff and then fed to a
  // reverse squelch. If the power is then BELOW a threshold, open the squelch.

  if (squelch_db != 0) {
    // Non-blocking as we are using squelch_two as a gate.
    squelch = gr::analog::pwr_squelch_cc::make(squelch_db, 0.01, 10, false);
    squelch->set_max_output_buffer(4096);
  }

  
  valve->set_max_output_buffer(4096);
  mixer->set_max_output_buffer(4096);
  lowpass_filter->set_max_output_buffer(4096);
  arb_resampler->set_max_output_buffer(4096);
  cutoff_filter->set_max_output_buffer(4096);

  connect(self(),      0, valve,         0);
  if (double_decim) {
    connect(valve,  0,  bandpass_filter, 0);
    connect(bandpass_filter, 0, mixer,   0);
    connect(bfo,    0,  mixer,           1);
  } else {
    connect(valve,  0,  mixer,           0);
    connect(lo,     0,  mixer,           1);
  }
  connect(mixer,    0,  lowpass_filter,   0);
  connect(lowpass_filter, 0,  arb_resampler, 0);
  connect(arb_resampler,  0,  cutoff_filter,  0);
}
void p25_recorder::initialize_fsk4() {
  const double phase1_channel_rate = phase1_symbol_rate * phase1_samples_per_symbol;
  const double pi = M_PI;

  // FSK4: Phase Loop Lock - can only be Phase 1, so locking at that rate.
  double freq_to_norm_radians = pi / (phase1_channel_rate / 2.0);
  double fc                   = 0.0;
  double fd                   = 600.0;
  double pll_demod_gain       = 1.0 / (fd * freq_to_norm_radians);
  pll_freq_lock = gr::analog::pll_freqdet_cf::make((phase1_symbol_rate / 2.0 * 1.2) * freq_to_norm_radians, (fc + (3 * fd * 1.9)) * freq_to_norm_radians, (fc + (-3 * fd * 1.9)) * freq_to_norm_radians);
  pll_amp       = gr::blocks::multiply_const_ff::make(pll_demod_gain * 1.0);

  //FSK4: noise filter - can only be Phase 1, so locking at that rate.
  baseband_noise_filter_taps = gr::filter::firdes::low_pass_2(1.0, phase1_channel_rate, phase1_symbol_rate / 2.0 * 1.175, phase1_symbol_rate / 2.0 * 0.125, 20.0, gr::filter::firdes::WIN_KAISER, 6.76);
  noise_filter               = gr::filter::fft_filter_fff::make(1.0, baseband_noise_filter_taps);

  //FSK4: Symbol Taps
  double symbol_decim = 1;

  for (int i = 0; i < samples_per_symbol; i++) {
    sym_taps.push_back(1.0 / samples_per_symbol);
  }
  sym_filter    =  gr::filter::fir_filter_fff::make(symbol_decim, sym_taps);

  //FSK4: FSK4 Demod - locked at Phase 1 rates, since it can only be Phase 1
  tune_queue    = gr::msg_queue::make(20);
  fsk4_demod = gr::op25_repeater::fsk4_demod_ff::make(tune_queue, phase1_channel_rate, phase1_symbol_rate);

  pll_freq_lock->set_max_output_buffer(4096);
  pll_amp->set_max_output_buffer(4096);
  noise_filter->set_max_output_buffer(4096);
  sym_filter->set_max_output_buffer(4096);
  fsk4_demod->set_max_output_buffer(4096);

  if (squelch_db != 0) {
    connect(cutoff_filter, 0, squelch,       0);
    connect(squelch,       0, pll_freq_lock, 0);
  } else {
    connect(cutoff_filter, 0, pll_freq_lock, 0);
  }
  connect(pll_freq_lock,        0, pll_amp,              0);
  connect(pll_amp,              0, noise_filter,         0);
  connect(noise_filter,         0, sym_filter,           0);
  connect(sym_filter,           0, fsk4_demod,           0);
  connect(fsk4_demod,           0, slicer,               0);
}

void p25_recorder::initialize_qpsk() {
  const double pi = M_PI;

  agc = gr::analog::feedforward_agc_cc::make(16, 1.0);


  // Gardner Costas Clock
  double gain_mu      = 0.025;               // 0.025
  double costas_alpha = 0.04;
  double omega      = double(system_channel_rate) / symbol_rate; // set to 6000 for TDMA, should be symbol_rate
  double gain_omega = 0.1  * gain_mu * gain_mu;
  double alpha      = costas_alpha;
  double beta       = 0.125 * alpha * alpha;
  double fmax       = 3000; // Hz
  fmax = 2 * pi * fmax / double(system_channel_rate);

  costas_clock = gr::op25_repeater::gardner_costas_cc::make(omega, gain_mu, gain_omega, alpha,  beta, fmax, -fmax);

  // QPSK: Perform Differential decoding on the constellation
  diffdec = gr::digital::diff_phasor_cc::make();

  // QPSK: take angle of the difference (in radians)
  to_float = gr::blocks::complex_to_arg::make();

  // QPSK: convert from radians such that signal is in -3/-1/+1/+3
  rescale = gr::blocks::multiply_const_ff::make((1 / (pi / 4)));

  agc->set_max_output_buffer(4096);
  costas_clock->set_max_output_buffer(4096);
  diffdec->set_max_output_buffer(4096);
  to_float->set_max_output_buffer(4096);
  rescale->set_max_output_buffer(4096);

  if (squelch_db != 0) {
    connect(cutoff_filter, 0, squelch, 0);
    connect(squelch,       0, agc,     0);
  } else {
    connect(cutoff_filter, 0, agc, 0);
  }
  connect(agc,                  0, costas_clock,         0);
  connect(costas_clock,         0, diffdec,              0);
  connect(diffdec,              0, to_float,             0);
  connect(to_float,             0, rescale,              0);
  connect(rescale,              0, slicer,               0);
}

void p25_recorder::initialize_p25() {
  //OP25 Slicer
  const float l[] = { -2.0, 0.0, 2.0, 4.0 };
  std::vector<float> slices(l, l + sizeof(l) / sizeof(l[0]));
  slicer     = gr::op25_repeater::fsk4_slicer_fb::make(slices);


  //OP25 Frame Assembler
  traffic_queue = gr::msg_queue::make(2);
  rx_queue      = gr::msg_queue::make(100);


  int udp_port               = 0;
  int verbosity              = 0; // 10 = lots of debug messages
  const char *wireshark_host = "127.0.0.1";
  bool do_imbe               = 1;
  bool do_output             = 1;
  bool do_msgq               = 0;
  bool do_audio_output       = 1;
  bool do_tdma               = 1;
  bool do_crypt              = 0;


  op25_frame_assembler = gr::op25_repeater::p25_frame_assembler::make(0, silence_frames, wireshark_host, udp_port, verbosity, do_imbe, do_output, do_msgq, rx_queue, do_audio_output, do_tdma, do_crypt);
  converter = gr::blocks::short_to_float::make(1, 32768.0);
  levels = gr::blocks::multiply_const_ff::make(source->get_digital_levels());

  slicer->set_max_output_buffer(4096);
  op25_frame_assembler->set_max_output_buffer(4096);
  converter->set_max_output_buffer(4096);
  levels->set_max_output_buffer(4096);

  connect(slicer,               0, op25_frame_assembler, 0);
  connect(op25_frame_assembler, 0, converter,            0);
  connect(converter,            0, levels,               0);
  connect(levels, 0, wav_sink, 0);
}
void p25_recorder::initialize(Source *src, gr::blocks::nonstop_wavfile_sink::sptr wav_sink)
{
  source      = src;
  chan_freq   = source->get_center();
  center_freq = source->get_center();
  config      = source->get_config();
  input_rate = source->get_rate();
  qpsk_mod       = source->get_qpsk_mod();
  silence_frames = source->get_silence_frames();
  squelch_db = source->get_squelch_db();
  this->wav_sink = wav_sink;
  talkgroup      = 0;
  d_phase2_tdma = false;
  rec_num = rec_counter++;
  recording_count = 0;
  recording_duration = 0;
  
  state = inactive;
  
  timestamp = time(NULL);
  starttime = time(NULL);

  initialize_prefilter();
  initialize_p25();

  if (!qpsk_mod) {
    initialize_fsk4();
  } else {
    initialize_qpsk();
  }
}

void p25_recorder::switch_tdma(bool phase2) {
  double omega;
  double fmax;
  const double pi = M_PI;

  if (phase2) {
    d_phase2_tdma = true;
    symbol_rate = 6000;
    samples_per_symbol = 4; //5;//4;
  } else {
    d_phase2_tdma = false;
    symbol_rate = 4800;
    samples_per_symbol = 5;
  }

  system_channel_rate = symbol_rate * samples_per_symbol;
  //decim = floor(initial_rate / system_channel_rate);
  //resampled_rate = double(initial_rate) / double(decim);
  arb_rate  = (double(system_channel_rate) / resampled_rate);
  BOOST_LOG_TRIVIAL(info) << "\t P25 Recorder - Adjust Channel Resampled Rate: " << resampled_rate  << " System Rate: " << system_channel_rate << " ARB Rate: " << arb_rate;

  generate_arb_taps();
  arb_resampler->set_rate(arb_rate);
  arb_resampler->set_taps(arb_taps);
  omega = double(system_channel_rate) / double(symbol_rate);
  fmax  = symbol_rate/2; // Hz
  fmax = 2 * pi * fmax / double(system_channel_rate);
  costas_clock->update_omega(omega);
  costas_clock->update_fmax(fmax);
  op25_frame_assembler->set_phase2_tdma(d_phase2_tdma);
}

void p25_recorder::set_tdma(bool phase2) {
  if (phase2 != d_phase2_tdma) {
    switch_tdma(phase2);
  }
}

void p25_recorder::clear() {
  //op25_frame_assembler->clear();
}

void p25_recorder::autotune() {
  if (!qpsk_mod) {
    gr::message::sptr msg;
    msg = tune_queue->delete_head_nowait();

    if (msg != 0) {
      BOOST_LOG_TRIVIAL(info) << "p25_recorder.cc: Freq:\t" << FormatFreq(chan_freq) << "\t Tune: " << msg->arg1();

      // tune_offset(freq + (msg->arg1()*100));
      tune_queue->flush();
    }
  }
}



p25_recorder::~p25_recorder() {}


Source * p25_recorder::get_source() {
  return source;
}

int p25_recorder::get_num() {
  return rec_num;
}

bool p25_recorder::is_active() {
  if (state == active) {
    return true;
  } else {
    return false;
  }
}

bool p25_recorder::is_idle() {
  if (state == active) {
    return !squelch->unmuted();
  }
  return true;
}

double p25_recorder::get_freq() {
  return chan_freq;
}

double p25_recorder::get_current_length() {
  return wav_sink->length_in_seconds();
}

int p25_recorder::lastupdate() {
  return time(NULL) - timestamp;
}

long p25_recorder::elapsed() {
  return time(NULL) - starttime;
}

void p25_recorder::tune_freq(double f) {
  chan_freq = f;
  float freq = (center_freq - f);
  tune_offset(freq);
}
void p25_recorder::tune_offset(double f) {
        
        float freq = static_cast<float> (f); 
        
        if (abs(freq) > ((input_rate/2) - (if1/2)))
        {
          BOOST_LOG_TRIVIAL(info) << "Tune Offset: Freq exceeds limit: " << abs(freq) << " compared to: " << ((input_rate/2) - (if1/2));
        }
        if (double_decim) {
          bandpass_filter_coeffs = gr::filter::firdes::complex_band_pass(1.0, input_rate, -freq - if1/2, -freq + if1/2, if1/2);
          bandpass_filter->set_taps(bandpass_filter_coeffs);
          float bfz = (static_cast<float> (decim) * -freq) / (float) input_rate;
          bfz = bfz - static_cast<int>(bfz);
          if (bfz < -0.5) {
            bfz = bfz + 1.0;
          }
          if (bfz > 0.5) {
            bfz = bfz - 1.0;
          }
          bfo->set_frequency(-bfz * if1);

    
        } else {
          lo->set_frequency(freq);
        }

  if (!qpsk_mod) {
    reset();
  } else {
    costas_clock->reset();
  }
  op25_frame_assembler->reset_rx_status();
}

State p25_recorder::get_state() {
  return state;
}

Rx_Status p25_recorder::get_rx_status() {
  return op25_frame_assembler->get_rx_status();
}
void p25_recorder::stop() {
  if (state == active) {
    recording_duration += wav_sink->length_in_seconds();
    clear();
    BOOST_LOG_TRIVIAL(info) << "\t- Stopping P25 Recorder Num [" << rec_num << "]\tTG: " << this->call->get_talkgroup_display() << "\tFreq: " << FormatFreq(chan_freq) << " \tTDMA: " << d_phase2_tdma << "\tSlot: " << tdma_slot;

    state = inactive;
    valve->set_enabled(false);
    wav_sink->close();
    //Rx_Status rx_status = op25_frame_assembler->get_rx_status();
    op25_frame_assembler->reset_rx_status();
  } else {
    BOOST_LOG_TRIVIAL(error) << "p25_recorder.cc: Trying to Stop an Inactive Logger!!!";
  }
}
void p25_recorder::reset() {
/*
  pll_freq_lock->update_gains();
  pll_freq_lock->frequency_limit();
  pll_freq_lock->phase_wrap();
  fsk4_demod->reset();*/
  //pll_demod->set_phase(0);

}

void p25_recorder::set_tdma_slot(int slot) {

  tdma_slot = slot;
  op25_frame_assembler->set_slotid(tdma_slot);

}



void p25_recorder::start(Call *call) {
  if (state == inactive) {
    timestamp = time(NULL);
    starttime = time(NULL);

    talkgroup = call->get_talkgroup();
    short_name = call->get_short_name();
    chan_freq      = call->get_freq();
    this->call = call;

    set_tdma(call->get_phase2_tdma());

    if (call->get_phase2_tdma()) {

      set_tdma_slot(call->get_tdma_slot());

      if (call->get_xor_mask()) {
        op25_frame_assembler->set_xormask(call->get_xor_mask());
      } else {
          BOOST_LOG_TRIVIAL(info) << "Error - can't set XOR Mask for TDMA";
      }
    } else {
      set_tdma_slot(0);
    }

    if (!qpsk_mod) {
      reset();
    }
    BOOST_LOG_TRIVIAL(info) << "\t- Starting P25 Recorder Num [" << rec_num << "]\tTG: " << this->call->get_talkgroup_display() << "\tFreq: " << FormatFreq(chan_freq) << " \tTDMA: " << call->get_phase2_tdma() << "\tSlot: " << call->get_tdma_slot();


    int offset_amount = (center_freq - chan_freq);
  
    tune_offset(offset_amount);

    wav_sink->open(call->get_filename());
    state = active;
    valve->set_enabled(true);
    wav_sink->set_call(call);
    recording_count++;
  } else {
    BOOST_LOG_TRIVIAL(error) << "p25_recorder.cc: Trying to Start an already Active Logger!!!";
  }
}
